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
    messaging::RQueue<DecisionRouteUpdate> decisionRouteUpdatesQueue,
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
      allAreas_{config->getAreaIds()} {
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
      // TODO: change persist store to use C++ struct prefixMap_
      prefixMap_[*entry.type_ref()][*entry.prefix_ref()] =
          PrefixEntry(entry, allAreas_);
      addPerfEventIfNotExist(
          addingEvents_[entry.type][entry.prefix], "LOADED_FROM_DISK");
    }
  }

  // Create initial timer to update all prefixes after HoldTime (2 * KA)
  initialSyncKvStoreTimer_ = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { syncKvStore(); });

  // Create throttled update state
  syncKvStoreThrottled_ = std::make_unique<AsyncThrottle>(
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
            LOG(INFO) << "Terminating prefix update request processing fiber";
            break;
          }

          auto& update = maybeUpdate.value();
          // if no specified dstination areas, apply to all areas
          std::unordered_set<std::string> dstAreas;
          if (update.dstAreas_ref()->empty()) {
            dstAreas = allAreas_;
          } else {
            for (const auto& area : *update.dstAreas_ref()) {
              dstAreas.emplace(area);
            }
          }

          switch (update.cmd) {
          case thrift::PrefixUpdateCommand::ADD_PREFIXES:
            advertisePrefixesImpl(*update.prefixes_ref(), dstAreas);
            break;
          case thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES:
            withdrawPrefixesImpl(*update.prefixes_ref());
            break;
          case thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES_BY_TYPE:
            CHECK(update.type_ref().has_value());
            withdrawPrefixesByTypeImpl(update.type_ref().value());
            break;
          case thrift::PrefixUpdateCommand::SYNC_PREFIXES_BY_TYPE:
            CHECK(update.type_ref().has_value());
            syncPrefixesByTypeImpl(
                update.type_ref().value(), *update.prefixes_ref(), dstAreas);
            break;
          default:
            LOG(FATAL) << "Unknown command received. "
                       << static_cast<int>(update.cmd);
            break;
          }
        }
      });

  // Fiber to process route updates from Decision
  addFiberTask(
      [q = std::move(decisionRouteUpdatesQueue), this]() mutable noexcept {
        while (true) {
          auto maybeThriftObj = q.get(); // perform read
          VLOG(1) << "Received route update from Decision";
          if (maybeThriftObj.hasError()) {
            LOG(INFO) << "Terminating route delta processing fiber";
            break;
          }

          processDecisionRouteUpdates(std::move(maybeThriftObj).value());
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
  for (const auto& area : allAreas_) {
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
PrefixManager::stop() {
  // Stop KvStoreClient first
  kvStoreClient_->stop();

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

void
PrefixManager::persistPrefixDb() {
  // prefixDb persistent entries have changed,
  // save the newest persistent entries to disk.
  thrift::PrefixDatabase persistentPrefixDb;
  persistentPrefixDb.thisNodeName = nodeId_;
  for (const auto& kv : prefixMap_) {
    for (const auto& [_, entry] : kv.second) {
      if (not entry.tPrefixEntry.ephemeral_ref().value_or(false)) {
        persistentPrefixDb.prefixEntries_ref()->emplace_back(
            entry.tPrefixEntry);
      }
    }
  }
  if (diskState_ != persistentPrefixDb) {
    configStore_->storeThriftObj(kPfxMgrConfigKey, persistentPrefixDb).get();
    diskState_ = std::move(persistentPrefixDb);
  }
}

std::unordered_set<std::string>
PrefixManager::updateKvStorePrefixEntry(PrefixEntry& entry) {
  std::unordered_set<std::string> prefixKeys;

  auto dstAreas = entry.dstAreas; // intended copy
  auto& prefixEntry = entry.tPrefixEntry;
  // prevent area_stack loop
  for (const auto fromArea : *prefixEntry.area_stack_ref()) {
    dstAreas.erase(fromArea);
  }

  for (const auto& toArea : dstAreas) {
    // TODO: run ingress policy
    auto prefixKey =
        PrefixKey(nodeId_, toIPNetwork(*prefixEntry.prefix_ref()), toArea)
            .getPrefixKey();
    auto prefixDb = createPrefixDb(nodeId_, {prefixEntry}, toArea);
    if (enablePerfMeasurement_) {
      prefixDb.perfEvents_ref() =
          addingEvents_[*prefixEntry.type_ref()][*prefixEntry.prefix_ref()];
    }
    auto prefixDbStr =
        fbzmq::util::writeThriftObjStr(std::move(prefixDb), serializer_);

    bool changed = kvStoreClient_->persistKey(
        prefixKey, prefixDbStr, ttlKeyInKvStore_, toArea);

    LOG_IF(INFO, changed) << "Advertising key: " << prefixKey
                          << " toArea KvStore area: " << toArea;
    prefixKeys.emplace(std::move(prefixKey));
  }
  return prefixKeys;
}

void
PrefixManager::syncKvStore() {
  std::vector<std::pair<std::string, std::string>> keyVals;
  std::unordered_set<std::string> nowAdvertisingKeys;
  std::unordered_set<thrift::IpPrefix> nowAdvertisingPrefixes;

  if (perPrefixKeys_) {
    for (auto& [type, prefixToInfo] : prefixMap_) {
      // TODO: tie break on prefix attributes
      for (auto& [prefix, entry] : prefixToInfo) {
        if (not nowAdvertisingPrefixes.count(prefix)) {
          addPerfEventIfNotExist(
              addingEvents_[type][prefix], "UPDATE_KVSTORE_THROTTLED");
          nowAdvertisingPrefixes.emplace(prefix);
          for (const auto& key : updateKvStorePrefixEntry(entry)) {
            nowAdvertisingKeys.emplace(key);
            keysToClear_.erase(key);
          }
        } else {
          addPerfEventIfNotExist(
              addingEvents_[type][prefix], "COVERED_BY_HIGHER_TYPE");
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
          prefixDb.prefixEntries_ref()->emplace_back(kv2.second.tPrefixEntry);
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
    for (const auto& area : allAreas_) {
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
    auto prefixKey = PrefixKey::fromStr(key);
    if (prefixKey.hasValue()) {
      // needed for backward compatibility
      thrift::PrefixEntry entry;
      entry.prefix_ref() = prefixKey.value().getIpPrefix();
      deletedPrefixDb.prefixEntries = {entry};
    }
    LOG(INFO) << "Withdrawing key: " << key
              << " from KvStore area: " << prefixKey->getPrefixArea();
    // one last key set with empty DB and deletePrefix set signifies withdraw
    // then the key should ttl out
    kvStoreClient_->clearKey(
        key,
        fbzmq::util::writeThriftObjStr(std::move(deletedPrefixDb), serializer_),
        ttlKeyInKvStore_,
        prefixKey->getPrefixArea());
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
  ]() mutable noexcept {
    p.setValue(advertisePrefixesImpl(prefixes, allAreas_));
  });
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
    p.setValue(syncPrefixesByTypeImpl(prefixType, prefixes, allAreas_));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
PrefixManager::getPrefixes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable noexcept {
    std::vector<thrift::PrefixEntry> prefixes;
    for (const auto& [_, prefixToInfo] : prefixMap_) {
      for (const auto& [_, entry] : prefixToInfo) {
        prefixes.emplace_back(entry.tPrefixEntry);
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
    if (prefixMap_.count(prefixType)) {
      for (const auto& [_, entry] : prefixMap_.at(prefixType)) {
        prefixes.emplace_back(entry.tPrefixEntry);
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
    const std::vector<thrift::PrefixEntry>& prefixes,
    const std::unordered_set<std::string>& dstAreas) {
  std::vector<PrefixEntry> toAddOrUpdate;
  for (const auto& prefix : prefixes) {
    toAddOrUpdate.emplace_back(prefix, dstAreas);
  }
  return advertisePrefixesImpl(toAddOrUpdate);
}

// helpers for modifying our Prefix Db
bool
PrefixManager::advertisePrefixesImpl(
    const std::vector<PrefixEntry>& prefixeInfos) {
  bool updated{false};

  for (const auto& entry : prefixeInfos) {
    const auto& type = *entry.tPrefixEntry.type_ref();
    const auto& prefix = *entry.tPrefixEntry.prefix_ref();

    auto& prefixes = prefixMap_[type];
    auto prefixIt = prefixes.find(prefix);

    // received same prefix entry, ignore
    if (prefixIt != prefixes.end() and prefixIt->second == entry) {
      continue;
    }

    if (prefixIt == prefixes.end()) {
      prefixes.emplace(prefix, entry);
      addPerfEventIfNotExist(addingEvents_[type][prefix], "ADD_PREFIX");
    } else {
      prefixIt->second = entry;
      addPerfEventIfNotExist(addingEvents_[type][prefix], "UPDATE_PREFIX");
    }
    updated = true;

    SYSLOG_IF(INFO, entry.dstAreas.size() > 0)
        << "Advertising prefix: " << toString(prefix) << " to area  "
        << folly::join(",", entry.dstAreas)
        << ", client: " << getPrefixTypeName(type);
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
    const std::vector<thrift::PrefixEntry>& prefixEntries,
    const std::unordered_set<std::string>& dstAreas) {
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
    toRemove.emplace_back(prefixMap_[type][prefix].tPrefixEntry);
  }
  bool updated = false;
  updated |= advertisePrefixesImpl(toAddOrUpdate, dstAreas);
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
PrefixManager::processDecisionRouteUpdates(
    DecisionRouteUpdate&& decisionRouteUpdate) {
  // if only one area is configured, no need to redisrtibute route
  // We want to keep processDecisionRouteUpdates() running as dynamic
  // configuration could add/remove areas.
  if (allAreas_.size() == 1) {
    return;
  }

  std::vector<PrefixEntry> advertisePrefixes;
  std::vector<thrift::PrefixEntry> withdrawPrefixes;

  // Add/Update unicast routes to update
  // Self originated (include routes imported from local BGP)
  // won't show up in decisionRouteUpdate.
  for (auto& route : decisionRouteUpdate.unicastRoutesToUpdate) {
    auto& prefixEntry = route.bestPrefixEntry;

    // NOTE: future expansion - run egress policy here

    // cross area, append area stack
    prefixEntry.area_stack_ref()->emplace_back(route.bestArea);
    // normalize to RIB routes
    prefixEntry.type_ref() = thrift::PrefixType::RIB;

    auto dstAreas = allAreas_;
    for (const auto& nh : route.nexthops) {
      dstAreas.erase(apache::thrift::can_throw(*nh.area_ref()));
    }

    advertisePrefixes.emplace_back(prefixEntry, dstAreas);
  }

  // Delete unicast routes
  for (const auto& prefix : decisionRouteUpdate.unicastRoutesToDelete) {
    withdrawPrefixes.emplace_back(
        createPrefixEntry(toIpPrefix(prefix), thrift::PrefixType::RIB));
  }

  advertisePrefixesImpl(advertisePrefixes);
  withdrawPrefixesImpl(withdrawPrefixes);

  // ignore mpls updates
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
