/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManager.h"

#include <fb303/ServiceData.h>
#include <folly/futures/Future.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/kvstore/KvStore.h>

namespace fb303 = facebook::fb303;

namespace openr {

namespace {
// key for the persist config on disk
const std::string kPfxMgrConfigKey{"prefix-manager-config"};
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
    messaging::RQueue<thrift::PrefixUpdateRequest> prefixUpdateRequestQueue,
    std::shared_ptr<const Config> config,
    PersistentStore* configStore,
    KvStore* kvStore,
    bool enablePerfMeasurement,
    const std::chrono::seconds& initialDumpTime,
    bool perPrefixKeys)
    : nodeId_(config->getNodeName()),
      configStore_{configStore},
      kvStore_(kvStore),
      perPrefixKeys_{perPrefixKeys},
      enablePerfMeasurement_{enablePerfMeasurement},
      ttlKeyInKvStore_(
          std::chrono::milliseconds(config->getKvStoreConfig().key_ttl_ms)),
      areas_{config->getAreaIds()} {
  CHECK(configStore_);
  CHECK(kvStore_);

  // Create KvStore client
  kvStoreClient_ =
      std::make_unique<KvStoreClientInternal>(this, nodeId_, kvStore_);

  // pick up prefixes from disk
  auto maybePrefixDb =
      configStore_->loadThriftObj<thrift::PrefixDatabase>(kPfxMgrConfigKey)
          .get();
  if (maybePrefixDb.hasValue()) {
    diskState_ = std::move(maybePrefixDb.value());
    LOG(INFO) << folly::sformat(
        "Successfully loaded {} prefixes from disk.",
        diskState_.prefixEntries.size());

    for (const auto& entry : diskState_.prefixEntries) {
      LOG(INFO) << folly::sformat(
          "  > {}, type {}",
          toString(entry.prefix),
          getPrefixTypeName(entry.type));
      prefixMap_[entry.type][entry.prefix] = entry;
      addPerfEventIfNotExist(
          addingEvents_[entry.type][entry.prefix], "LOADED_FROM_DISK");
    }
  }

  // Create initial timer to update all prefixes after HoldTime (2 * KA)
  initialSyncKvStoreTimer_ =
      fbzmq::ZmqTimeout::make(getEvb(), [this]() noexcept { syncKvStore(); });

  // Create throttled update state
  syncKvStoreThrottled_ = std::make_unique<fbzmq::ZmqThrottle>(
      getEvb(), Constants::kPrefixMgrKvThrottleTimeout, [this]() noexcept {
        if (initialSyncKvStoreTimer_->isScheduled()) {
          return;
        }
        syncKvStore();
      });

  // Schedule fiber to read prefix updates messages
  addFiberTask(
      [q = std::move(prefixUpdateRequestQueue), this]() mutable noexcept {
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
            advertisePrefixesImpl(update.prefixes);
            break;
          case thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES:
            withdrawPrefixesImpl(update.prefixes);
            break;
          case thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES_BY_TYPE:
            CHECK(update.type_ref().has_value());
            withdrawPrefixesByTypeImpl(update.type_ref().value());
            break;
          case thrift::PrefixUpdateCommand::SYNC_PREFIXES_BY_TYPE:
            CHECK(update.type_ref().has_value());
            syncPrefixesByTypeImpl(update.type_ref().value(), update.prefixes);
            break;
          default:
            LOG(FATAL) << "Unknown command received. "
                       << static_cast<int>(update.cmd);
            break;
          }
        }
      });

  // register kvstore publication callback
  std::vector<std::string> keyPrefixList = {
      Constants::kPrefixDbMarker.toString() + nodeId_};
  kvStoreClient_->subscribeKeyFilter(
      KvStoreFilters(keyPrefixList, {} /* originatorIds*/),
      [this](
          const std::string& key, std::optional<thrift::Value> value) noexcept {
        // we're not currently persisting this key, it may be that we no longer
        // want it advertised
        if (value.has_value() and value.value().value_ref().has_value()) {
          const auto prefixDb =
              fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
                  value.value().value_ref().value(), serializer_);
          if (not prefixDb.deletePrefix && nodeId_ == prefixDb.thisNodeName) {
            LOG(INFO) << "keysToClear_.emplace(" << key << ")";
            keysToClear_.emplace(key);
            syncKvStoreThrottled_->operator()();
          }
        }
      });

  // get initial dump of keys related to us
  for (const auto& area : areas_) {
    auto result =
        kvStoreClient_->dumpAllWithPrefix(keyPrefixList.front(), area);
    if (!result.has_value()) {
      LOG(ERROR) << "Failed dumping keys with prefix: " << keyPrefixList.front()
                 << " from area: " << area;
      continue;
    }
    for (auto const& kv : result.value()) {
      keysToClear_.emplace(kv.first);
    }
  }

  // initialDumpTime zero is used during testing to do inline without delay
  initialSyncKvStoreTimer_->scheduleTimeout(initialDumpTime);
}

PrefixManager::~PrefixManager() {
  // - If EventBase is stopped or it is within the evb thread, run immediately;
  // - Otherwise, will wait the EventBase to run;
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    // destory timers
    LOG(INFO) << "Destroyed timers inside PrefixManager";
    initialSyncKvStoreTimer_.reset();
    syncKvStoreThrottled_.reset();
  });
  kvStoreClient_.reset();
}

void
PrefixManager::persistPrefixDb() {
  // prefixDb persistent entries have changed,
  // save the newest persistent entries to disk.
  thrift::PrefixDatabase persistentPrefixDb;
  persistentPrefixDb.thisNodeName = nodeId_;
  for (const auto& kv : prefixMap_) {
    for (const auto& kv2 : kv.second) {
      if (not kv2.second.ephemeral_ref().value_or(false)) {
        persistentPrefixDb.prefixEntries.emplace_back(kv2.second);
      }
    }
  }
  if (diskState_ != persistentPrefixDb) {
    configStore_->storeThriftObj(kPfxMgrConfigKey, persistentPrefixDb).get();
    diskState_ = std::move(persistentPrefixDb);
  }
}

std::string
PrefixManager::updateKvStorePrefixEntry(thrift::PrefixEntry& prefixEntry) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeId_;
  prefixDb.prefixEntries.emplace_back(prefixEntry);
  if (enablePerfMeasurement_) {
    prefixDb.perfEvents_ref() =
        addingEvents_[prefixEntry.type][prefixEntry.prefix];
  }
  const auto prefixKey =
      PrefixKey(
          nodeId_,
          folly::IPAddress::createNetwork(toString(prefixEntry.prefix)),
          thrift::KvStore_constants::kDefaultArea())
          .getPrefixKey();
  for (const auto& area : areas_) {
    bool const changed = kvStoreClient_->persistKey(
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
PrefixManager::syncKvStore() {
  std::vector<std::pair<std::string, std::string>> keyVals;
  std::unordered_set<std::string> nowAdvertisingKeys;
  std::unordered_set<thrift::IpPrefix> nowAdvertisingPrefixes;

  if (perPrefixKeys_) {
    for (auto& kv : prefixMap_) {
      for (auto& kv2 : kv.second) {
        if (not nowAdvertisingPrefixes.count(kv2.first)) {
          addPerfEventIfNotExist(
              addingEvents_[kv.first][kv2.first], "UPDATE_KVSTORE_THROTTLED");
          auto const key = updateKvStorePrefixEntry(kv2.second);
          nowAdvertisingKeys.emplace(key);
          nowAdvertisingPrefixes.emplace(kv2.first);
          keysToClear_.erase(key);
        } else {
          addPerfEventIfNotExist(
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
          addPerfEventIfNotExist(
              addingEvents_[kv.first][kv2.first], "UPDATE_KVSTORE_THROTTLED");
          if (nullptr == mostRecentEvents or
              addingEvents_[kv.first][kv2.first].events.back().unixTs >
                  mostRecentEvents->events.back().unixTs) {
            mostRecentEvents = &addingEvents_[kv.first][kv2.first];
          }
          prefixDb.prefixEntries.emplace_back(kv2.second);
          nowAdvertisingPrefixes.emplace(kv2.first);
        } else {
          addPerfEventIfNotExist(
              addingEvents_[kv.first][kv2.first], "COVERED_BY_HIGHER_TYPE");
        }
      }
    }
    if (enablePerfMeasurement_ and nullptr != mostRecentEvents) {
      prefixDb.perfEvents_ref() = *mostRecentEvents;
    }
    const auto prefixDbKey =
        folly::sformat("{}{}", Constants::kPrefixDbMarker.toString(), nodeId_);
    for (const auto& area : areas_) {
      bool const changed = kvStoreClient_->persistKey(
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
    deletedPrefixDb.perfEvents_ref() = thrift::PerfEvents{};
    addPerfEventIfNotExist(
        deletedPrefixDb.perfEvents_ref().value(), "WITHDRAW_THROTTLED");
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
      kvStoreClient_->clearKey(
          key,
          fbzmq::util::writeThriftObjStr(
              std::move(deletedPrefixDb), serializer_),
          ttlKeyInKvStore_,
          area);
    }
  }

  // anything we don't advertise next time, we need to clear
  keysToClear_ = std::move(nowAdvertisingKeys);

  // Update flat counters
  size_t num_prefixes = 0;
  for (auto const& kv : prefixMap_) {
    fb303::fbData->setCounter(
        "prefix_manager.num_prefixes." + getPrefixTypeName(kv.first),
        kv.second.size());
    num_prefixes += kv.second.size();
  }
  fb303::fbData->setCounter("prefix_manager.num_prefixes", num_prefixes);
}

folly::SemiFuture<bool>
PrefixManager::advertisePrefixes(std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([
    this,
    p = std::move(p),
    prefixes = std::move(prefixes)
  ]() mutable noexcept { p.setValue(advertisePrefixesImpl(prefixes)); });
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
  ]() mutable noexcept { p.setValue(withdrawPrefixesImpl(prefixes)); });
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
  ]() mutable noexcept { p.setValue(withdrawPrefixesByTypeImpl(prefixType)); });
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
  ]() mutable noexcept {
    p.setValue(syncPrefixesByTypeImpl(prefixType, prefixes));
  });
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

// helpers for modifying our Prefix Db
bool
PrefixManager::advertisePrefixesImpl(
    const std::vector<thrift::PrefixEntry>& prefixEntries) {
  bool updated{false};
  for (const auto& prefixEntry : prefixEntries) {
    auto& prefixes = prefixMap_[prefixEntry.type];
    auto it = prefixes.find(prefixEntry.prefix);
    if (it == prefixes.end() or it->second != prefixEntry) {
      prefixes[prefixEntry.prefix] = prefixEntry;
      addPerfEventIfNotExist(
          addingEvents_[prefixEntry.type][prefixEntry.prefix],
          it == prefixes.end() ? "ADD_PREFIX" : "UPDATE_PREFIX");
      updated = true;
      SYSLOG(INFO) << "Advertising prefix: " << toString(prefixEntry.prefix)
                   << ", client: " << getPrefixTypeName(prefixEntry.type);
    }
  }
  if (updated) {
    persistPrefixDb();
    syncKvStoreThrottled_->operator()();
  }
  return updated;
}

bool
PrefixManager::withdrawPrefixesImpl(
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
    syncKvStoreThrottled_->operator()();
  }
  return !prefixes.empty();
}

bool
PrefixManager::syncPrefixesByTypeImpl(
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
  updated |= advertisePrefixesImpl(toAddOrUpdate);
  updated |= withdrawPrefixesImpl(toRemove);
  return updated;
}

bool
PrefixManager::withdrawPrefixesByTypeImpl(thrift::PrefixType type) {
  bool changed = false;
  auto const search = prefixMap_.find(type);
  if (search != prefixMap_.end()) {
    changed = true;
    prefixMap_.erase(search);
  }
  if (changed) {
    persistPrefixDb();
    syncKvStoreThrottled_->operator()();
  }
  return changed;
}

void
PrefixManager::addPerfEventIfNotExist(
    thrift::PerfEvents& perfEvents, std::string const& updateEvent) {
  if (perfEvents.events.empty() or
      perfEvents.events.back().eventDescr != updateEvent) {
    addPerfEvent(perfEvents, nodeId_, updateEvent);
  }
}

} // namespace openr
