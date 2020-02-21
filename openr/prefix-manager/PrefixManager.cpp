/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManager.h"

#include <folly/futures/Future.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

namespace {
// key for the persist config on disk
const std::string kConfigKey{"prefix-manager-config"};
// various error messages
const std::string kErrorNoChanges{"No changes in prefixes to be advertised"};
const std::string kErrorNoPrefixToRemove{"No prefix to remove"};
const std::string kErrorNoPrefixesOfType{"No prefixes of type"};
const std::string kErrorUnknownCommand{"Unknown command"};

std::string
getPrefixTypeName(thrift::PrefixType const& type) {
  return apache::thrift::TEnumTraits<thrift::PrefixType>::findName(type);
}

} // namespace

PrefixManager::PrefixManager(
    const std::string& nodeId,
    messaging::RQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue,
    PersistentStore* configStore,
    const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    const PrefixDbMarker& prefixDbMarker,
    bool perPrefixKeys,
    bool enablePerfMeasurement,
    const std::chrono::seconds prefixHoldTime,
    const std::chrono::milliseconds ttlKeyInKvStore,
    fbzmq::Context& zmqContext,
    const std::unordered_set<std::string>& areas)
    : nodeId_(nodeId),
      configStore_{configStore},
      prefixDbMarker_{prefixDbMarker},
      perPrefixKeys_{perPrefixKeys},
      enablePerfMeasurement_{enablePerfMeasurement},
      ttlKeyInKvStore_(ttlKeyInKvStore),
      kvStoreClient_{
          zmqContext, this, nodeId_, kvStoreLocalCmdUrl, kvStoreLocalPubUrl},
      areas_{areas} {
  CHECK(configStore_);
  // pick up prefixes from disk
  auto maybePrefixDb =
      configStore_->loadThriftObj<thrift::PrefixDatabase>(kConfigKey).get();
  if (maybePrefixDb.hasValue()) {
    LOG(INFO) << "Successfully loaded " << maybePrefixDb->prefixEntries.size()
              << " prefixes from disk";
    diskState_ = std::move(maybePrefixDb.value());
    for (const auto& entry : diskState_.prefixEntries) {
      LOG(INFO) << "  > " << toString(entry.prefix) << ", type "
                << getPrefixTypeName(entry.type);
      prefixMap_[entry.type][entry.prefix] = entry;
      addPerfEvent(
          addingEvents_[entry.type][entry.prefix], nodeId_, "LOADED_FROM_DISK");
    }
  }
  // Create throttled update state
  outputStateThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      getEvb(), Constants::kPrefixMgrKvThrottleTimeout, [this]() noexcept {
        outputState();
      });

  // Schedule fiber to read prefix updates messages
  addFiberTask([q = std::move(prefixUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeUpdate = q.get(); // perform read
      VLOG(1) << "Received prefix update request";
      if (maybeUpdate.hasError()) {
        LOG(INFO) << "Terminating route delta processing fiber";
        break;
      }

      auto& update = maybeUpdate.value();
      switch (update.cmd) {
      case thrift::PrefixUpdateCommand::ADD_PREFIXES:
        addOrUpdatePrefixes(update.prefixes);
        break;
      case thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES:
        removePrefixes(update.prefixes);
        break;
      case thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES_BY_TYPE:
        CHECK(update.type.hasValue());
        removePrefixesByType(update.type.value());
        break;
      case thrift::PrefixUpdateCommand::SYNC_PREFIXES_BY_TYPE:
        CHECK(update.type.hasValue());
        syncPrefixes(update.type.value(), update.prefixes);
        break;
      default:
        LOG(FATAL) << "Unknown command received. "
                   << static_cast<int>(update.cmd);
        break;
      }
    }
  });

  // register kvstore publication callback
  std::vector<std::string> keyPrefixList;
  keyPrefixList.emplace_back(folly::sformat(
      "{}{}", static_cast<std::string>(prefixDbMarker_), nodeId_));
  std::set<std::string> originatorIds{};
  KvStoreFilters kvFilters = KvStoreFilters(keyPrefixList, originatorIds);
  kvStoreClient_.subscribeKeyFilter(
      std::move(kvFilters),
      [this](
          const std::string& key,
          folly::Optional<thrift::Value> value) noexcept {
        // we're not currently persisting this key, it may be that we no longer
        // want it advertised
        if (value.hasValue() and value.value().value.has_value()) {
          const auto prefixDb =
              fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
                  value.value().value.value(), serializer_);
          if (not prefixDb.deletePrefix && nodeId_ == prefixDb.thisNodeName) {
            LOG(INFO) << "keysToClear_.emplace(" << key << ")";
            keysToClear_.emplace(key);
            outputStateThrottled_->operator()();
          }
        }
      });

  // get initial dump of keys related to us
  for (const auto& area : areas_) {
    auto result = kvStoreClient_.dumpAllWithPrefix(keyPrefixList.front(), area);
    if (result.hasError()) {
      LOG(ERROR) << "Failed dumping keys from area " << area << " :"
                 << result.error();
      continue;
    }
    for (auto const& kv : result.value()) {
      keysToClear_.emplace(kv.first);
    }
  }

  // Create a timer to update all prefixes after HoldTime (2 * KA) during
  // initial start up
  // Holdtime zero is used during testing to do inline without delay
  initialOutputStateTimer_ =
      fbzmq::ZmqTimeout::make(getEvb(), [this]() noexcept { outputState(); });
  initialOutputStateTimer_->scheduleTimeout(prefixHoldTime);

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ = fbzmq::ZmqTimeout::make(
      getEvb(), [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);
}

void
PrefixManager::stop() {
  prefixUpdatesTaskFuture_.wait();
  OpenrEventBase::stop();
}

void
PrefixManager::outputState() {
  if (initialOutputStateTimer_->isScheduled()) {
    return;
  }
  updateKvStore();
}

void
PrefixManager::persistPrefixDb() {
  // prefixDb persistent entries have changed,
  // save the newest persistent entries to disk.
  thrift::PrefixDatabase persistentPrefixDb;
  persistentPrefixDb.thisNodeName = nodeId_;
  for (const auto& kv : prefixMap_) {
    for (const auto& kv2 : kv.second) {
      if (not kv2.second.ephemeral.value_or(false)) {
        persistentPrefixDb.prefixEntries.emplace_back(kv2.second);
      }
    }
  }
  if (diskState_ != persistentPrefixDb) {
    configStore_->storeThriftObj(kConfigKey, persistentPrefixDb).get();
    diskState_ = std::move(persistentPrefixDb);
  }
}

std::string
PrefixManager::advertisePrefix(thrift::PrefixEntry& prefixEntry) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeId_;
  prefixDb.prefixEntries.emplace_back(prefixEntry);
  if (enablePerfMeasurement_) {
    prefixDb.perfEvents = addingEvents_[prefixEntry.type][prefixEntry.prefix];
  }
  const auto prefixKey =
      PrefixKey(
          nodeId_,
          folly::IPAddress::createNetwork(toString(prefixEntry.prefix)),
          thrift::KvStore_constants::kDefaultArea())
          .getPrefixKey();
  for (const auto& area : areas_) {
    bool const changed = kvStoreClient_.persistKey(
        prefixKey,
        fbzmq::util::writeThriftObjStr(std::move(prefixDb), serializer_),
        ttlKeyInKvStore_,
        area);
    LOG_IF(INFO, changed) << "Advertising key: " << prefixKey
                          << " to KvStore area: " << area;
  }
  return prefixKey;
}

void
PrefixManager::updateKvStore() {
  std::vector<std::pair<std::string, std::string>> keyVals;
  std::unordered_set<std::string> nowAdvertisingKeys;
  std::unordered_set<thrift::IpPrefix> nowAdvertisingPrefixes;
  if (perPrefixKeys_) {
    for (auto& kv : prefixMap_) {
      for (auto& kv2 : kv.second) {
        if (not nowAdvertisingPrefixes.count(kv2.first)) {
          maybeAddEvent(
              addingEvents_[kv.first][kv2.first], "UPDATE_KVSTORE_THROTTLED");
          auto const key = advertisePrefix(kv2.second);
          nowAdvertisingKeys.emplace(key);
          nowAdvertisingPrefixes.emplace(kv2.first);
          keysToClear_.erase(key);
        } else {
          maybeAddEvent(
              addingEvents_[kv.first][kv2.first], "COVERED_BY_HIGHER_TYPE");
        }
      }
    }
  } else {
    thrift::PrefixDatabase prefixDb;
    prefixDb.thisNodeName = nodeId_;
    thrift::PerfEvents* mostRecentEvents = nullptr;
    for (auto& kv : prefixMap_) {
      for (auto& kv2 : kv.second) {
        if (not nowAdvertisingPrefixes.count(kv2.first)) {
          maybeAddEvent(
              addingEvents_[kv.first][kv2.first], "UPDATE_KVSTORE_THROTTLED");
          if (nullptr == mostRecentEvents or
              addingEvents_[kv.first][kv2.first].events.back().unixTs >
                  mostRecentEvents->events.back().unixTs) {
            mostRecentEvents = &addingEvents_[kv.first][kv2.first];
          }
          prefixDb.prefixEntries.emplace_back(kv2.second);
          nowAdvertisingPrefixes.emplace(kv2.first);
        } else {
          maybeAddEvent(
              addingEvents_[kv.first][kv2.first], "COVERED_BY_HIGHER_TYPE");
        }
      }
    }
    if (enablePerfMeasurement_ and nullptr != mostRecentEvents) {
      prefixDb.perfEvents = *mostRecentEvents;
    }
    const auto prefixDbKey = folly::sformat(
        "{}{}", static_cast<std::string>(prefixDbMarker_), nodeId_);
    for (const auto& area : areas_) {
      bool const changed = kvStoreClient_.persistKey(
          prefixDbKey,
          fbzmq::util::writeThriftObjStr(std::move(prefixDb), serializer_),
          ttlKeyInKvStore_,
          area);
      LOG_IF(INFO, changed)
          << "Updating all " << prefixDb.prefixEntries.size()
          << " prefixes in KvStore " << prefixDbKey << " area: " << area;
    }
    nowAdvertisingKeys.emplace(prefixDbKey);
    keysToClear_.erase(prefixDbKey);
  }
  thrift::PrefixDatabase deletedPrefixDb;
  deletedPrefixDb.thisNodeName = nodeId_;
  deletedPrefixDb.deletePrefix = true;
  if (enablePerfMeasurement_) {
    deletedPrefixDb.perfEvents = thrift::PerfEvents{};
    maybeAddEvent(deletedPrefixDb.perfEvents.value(), "WITHDRAW_THROTTLED");
  }
  for (auto const& key : keysToClear_) {
    auto maybePerPrefixKey = PrefixKey::fromStr(key);
    if (maybePerPrefixKey.hasValue()) {
      // needed for backward compatibility
      thrift::PrefixEntry entry;
      entry.prefix = maybePerPrefixKey.value().getIpPrefix();
      deletedPrefixDb.prefixEntries = {entry};
    }
    for (const auto& area : areas_) {
      LOG(INFO) << "Withdrawing key: " << key << " from KvStore area: " << area;
      // one last key set with empty DB and deletePrefix set signifies withdraw
      // then the key should ttl out
      kvStoreClient_.clearKey(
          key,
          fbzmq::util::writeThriftObjStr(
              std::move(deletedPrefixDb), serializer_),
          ttlKeyInKvStore_,
          area);
    }
  }

  // anything we don't advertise next time, we need to clear
  keysToClear_ = std::move(nowAdvertisingKeys);
}

folly::SemiFuture<bool>
PrefixManager::advertisePrefixes(std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([
    this,
    p = std::move(p),
    prefixes = std::move(prefixes)
  ]() mutable noexcept { p.setValue(addOrUpdatePrefixes(prefixes)); });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::withdrawPrefixes(std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([
    this,
    p = std::move(p),
    prefixes = std::move(prefixes)
  ]() mutable noexcept { p.setValue(removePrefixes(prefixes)); });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::withdrawPrefixesByType(thrift::PrefixType prefixType) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([
    this,
    p = std::move(p),
    prefixType = std::move(prefixType)
  ]() mutable noexcept { p.setValue(removePrefixesByType(prefixType)); });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::syncPrefixesByType(
    thrift::PrefixType prefixType, std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([
    this,
    p = std::move(p),
    prefixType = std::move(prefixType),
    prefixes = std::move(prefixes)
  ]() mutable noexcept { p.setValue(syncPrefixes(prefixType, prefixes)); });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
PrefixManager::getPrefixes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable noexcept {
    std::vector<thrift::PrefixEntry> prefixes;
    for (const auto& kv : prefixMap_) {
      for (const auto& kv2 : kv.second) {
        prefixes.emplace_back(kv2.second);
      }
    }
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
        std::move(prefixes)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
PrefixManager::getPrefixesByType(thrift::PrefixType prefixType) {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([
    this,
    p = std::move(p),
    prefixType = std::move(prefixType)
  ]() mutable noexcept {
    std::vector<thrift::PrefixEntry> prefixes;
    auto const search = prefixMap_.find(prefixType);
    if (search != prefixMap_.end()) {
      for (const auto& kv : search->second) {
        prefixes.emplace_back(kv.second);
      }
    }
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
        std::move(prefixes)));
  });
  return sf;
}

void
PrefixManager::submitCounters() {
  VLOG(2) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();
  counters["prefix_manager.zmq_event_queue_size"] =
      getEvb()->getNotificationQueueSize();

  // Count total route number
  size_t num_prefixes = 0;
  for (auto const& kv : prefixMap_) {
    counters[folly::sformat(
        "prefix_manager.num_prefixes.{}", getPrefixTypeName(kv.first))] =
        kv.second.size();
    num_prefixes += kv.second.size();
  }
  counters["prefix_manager.num_prefixes"] = num_prefixes;

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

// helpers for modifying our Prefix Db
bool
PrefixManager::addOrUpdatePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixEntries) {
  bool updated{false};
  for (const auto& prefixEntry : prefixEntries) {
    auto& prefixes = prefixMap_[prefixEntry.type];
    auto it = prefixes.find(prefixEntry.prefix);
    if (it == prefixes.end() or it->second != prefixEntry) {
      prefixes[prefixEntry.prefix] = prefixEntry;
      addPerfEvent(
          addingEvents_[prefixEntry.type][prefixEntry.prefix],
          nodeId_,
          it == prefixes.end() ? "ADD_PREFIX" : "UPDATE_PREFIX");
      updated = true;
      SYSLOG(INFO) << "Advertising prefix: " << toString(prefixEntry.prefix)
                   << ", client: " << getPrefixTypeName(prefixEntry.type);
    }
  }
  if (updated) {
    persistPrefixDb();
    outputStateThrottled_->operator()();
  }
  return updated;
}

bool
PrefixManager::removePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  // verify prefixes exists
  for (const auto& prefix : prefixes) {
    auto it = prefixMap_[prefix.type].find(prefix.prefix);
    if (it == prefixMap_[prefix.type].end()) {
      LOG(ERROR) << "Cannot withdraw prefix: " << toString(prefix.prefix)
                 << ", client: " << getPrefixTypeName(prefix.type);
      return false;
    }
  }
  for (const auto& prefix : prefixes) {
    prefixMap_.at(prefix.type).erase(prefix.prefix);
    addingEvents_.at(prefix.type).erase(prefix.prefix);
    SYSLOG(INFO) << "Withdrawing prefix: " << toString(prefix.prefix)
                 << ", client: " << getPrefixTypeName(prefix.type);
    if (prefixMap_[prefix.type].empty()) {
      prefixMap_.erase(prefix.type);
    }
    if (addingEvents_[prefix.type].empty()) {
      addingEvents_.erase(prefix.type);
    }
  }
  if (!prefixes.empty()) {
    persistPrefixDb();
    outputStateThrottled_->operator()();
  }
  return !prefixes.empty();
}

bool
PrefixManager::syncPrefixes(
    thrift::PrefixType type,
    const std::vector<thrift::PrefixEntry>& prefixEntries) {
  LOG(INFO) << "Syncing prefixes of type: " << getPrefixTypeName(type);
  // building these lists so we can call add and remove and get detailed logging
  std::vector<thrift::PrefixEntry> toAddOrUpdate, toRemove;
  std::unordered_set<thrift::IpPrefix> toRemoveSet;
  for (auto const& kv : prefixMap_[type]) {
    toRemoveSet.emplace(kv.first);
  }
  for (auto const& entry : prefixEntries) {
    CHECK(type == entry.type);
    toRemoveSet.erase(entry.prefix);
    toAddOrUpdate.emplace_back(entry);
  }
  for (auto const& prefix : toRemoveSet) {
    toRemove.emplace_back(prefixMap_[type][prefix]);
  }
  bool updated = false;
  updated |= addOrUpdatePrefixes(toAddOrUpdate);
  updated |= removePrefixes(toRemove);
  return updated;
}

bool
PrefixManager::removePrefixesByType(thrift::PrefixType type) {
  bool changed = false;
  auto const search = prefixMap_.find(type);
  if (search != prefixMap_.end()) {
    changed = true;
    prefixMap_.erase(search);
  }
  if (changed) {
    persistPrefixDb();
    outputStateThrottled_->operator()();
  }
  return changed;
}

void
PrefixManager::maybeAddEvent(
    thrift::PerfEvents& perfEvents, std::string const& updateEvent) {
  if (perfEvents.events.empty() or
      perfEvents.events.back().eventDescr != updateEvent) {
    addPerfEvent(perfEvents, nodeId_, updateEvent);
  }
}

} // namespace openr
