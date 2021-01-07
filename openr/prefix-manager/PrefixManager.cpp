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
#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/kvstore/KvStore.h>

namespace fb303 = facebook::fb303;

namespace openr {

PrefixManager::PrefixManager(
    messaging::ReplicateQueue<thrift::RouteDatabaseDelta>&
        staticRouteUpdatesQueue,
    messaging::RQueue<PrefixEvent> prefixUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> decisionRouteUpdatesQueue,
    std::shared_ptr<const Config> config,
    KvStore* kvStore,
    bool enablePerfMeasurement,
    const std::chrono::seconds& initialDumpTime)
    : nodeId_(config->getNodeName()),
      staticRouteUpdatesQueue_(staticRouteUpdatesQueue),
      kvStore_(kvStore),
      enablePerfMeasurement_{enablePerfMeasurement},
      ttlKeyInKvStore_(std::chrono::milliseconds(
          *config->getKvStoreConfig().key_ttl_ms_ref())),
      allAreas_{config->getAreaIds()} {
  CHECK(kvStore_);
  CHECK(config);

  // Create KvStore client
  kvStoreClient_ =
      std::make_unique<KvStoreClientInternal>(this, nodeId_, kvStore_);

  // Load openrConfig for local-originated routes
  if (auto prefixes = config->getConfig().originated_prefixes_ref()) {
    buildOriginatedPrefixDb(*prefixes);
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
  addFiberTask([q = std::move(prefixUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeUpdate = q.get(); // perform read
      if (maybeUpdate.hasError()) {
        LOG(INFO) << "Terminating prefix update request processing fiber";
        break;
      }
      auto& update = maybeUpdate.value();

      // if no specified dstination areas, apply to all areas
      std::unordered_set<std::string> dstAreas;
      if (update.dstAreas.empty()) {
        dstAreas = allAreas_;
      } else {
        for (const auto& area : update.dstAreas) {
          dstAreas.emplace(area);
        }
      }

      switch (update.eventType) {
      case PrefixEventType::ADD_PREFIXES:
        advertisePrefixesImpl(update.prefixes, dstAreas);
        break;
      case PrefixEventType::WITHDRAW_PREFIXES:
        withdrawPrefixesImpl(update.prefixes);
        break;
      case PrefixEventType::WITHDRAW_PREFIXES_BY_TYPE:
        CHECK(update.type.has_value());
        withdrawPrefixesByTypeImpl(update.type.value());
        break;
      case PrefixEventType::SYNC_PREFIXES_BY_TYPE:
        CHECK(update.type.has_value());
        syncPrefixesByTypeImpl(update.type.value(), update.prefixes, dstAreas);
        break;
      default:
        LOG(ERROR) << "Unknown command received. "
                   << static_cast<int>(update.eventType);
      }
    }
  });

  // Fiber to process route updates from Decision
  addFiberTask(
      [q = std::move(decisionRouteUpdatesQueue), this]() mutable noexcept {
        while (true) {
          auto maybeThriftObj = q.get(); // perform read
          if (maybeThriftObj.hasError()) {
            LOG(INFO) << "Terminating route delta processing fiber";
            break;
          }

          try {
            VLOG(2) << "Received RIB updates from Decision";
            processDecisionRouteUpdates(std::move(maybeThriftObj).value());
          } catch (const std::exception&) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
            // collect stack strace then fail the process
            for (auto& exInfo :
                 folly::exception_tracer::getCurrentExceptions()) {
              LOG(ERROR) << exInfo;
            }
#endif
            throw;
          }
        }
      });

  // register kvstore publication callback
  std::vector<std::string> const keyPrefixList = {
      Constants::kPrefixDbMarker.toString() + nodeId_};
  kvStoreClient_->subscribeKeyFilter(
      KvStoreFilters(keyPrefixList, {} /* originatorIds */),
      [this](
          const std::string& key, std::optional<thrift::Value> value) noexcept {
        // we're not currently persisting this key, it may be that we no longer
        // want it advertised
        if (value.has_value() and value.value().value_ref().has_value()) {
          const auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
              value.value().value_ref().value(), serializer_);
          if (not(*prefixDb.deletePrefix_ref()) &&
              nodeId_ == *prefixDb.thisNodeName_ref()) {
            VLOG(2) << "Learning previously announce route, key: " << key;
            keysToClear_.emplace(key);
            syncKvStoreThrottled_->operator()();
          }
        }
      });

  // get initial dump of keys related to us
  for (const auto& area : allAreas_) {
    auto result =
        kvStoreClient_->dumpAllWithPrefix(AreaId{area}, keyPrefixList.front());
    if (!result.has_value()) {
      LOG(ERROR) << "Failed dumping keys with prefix " << keyPrefixList.front()
                 << " from area " << area;
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
PrefixManager::buildOriginatedPrefixDb(
    const std::vector<thrift::OriginatedPrefix>& prefixes) {
  for (const auto& prefix : prefixes) {
    auto network = folly::IPAddress::createNetwork(*prefix.prefix_ref());
    auto nh = network.first.isV4() ? Constants::kLocalRouteNexthopV4.toString()
                                   : Constants::kLocalRouteNexthopV6.toString();

    // Populate PrefixMetric struct
    thrift::PrefixMetrics metrics;
    if (auto pref = prefix.path_preference_ref()) {
      metrics.path_preference_ref() = *pref;
    }
    if (auto pref = prefix.source_preference_ref()) {
      metrics.source_preference_ref() = *pref;
    }

    // Populate PrefixEntry struct
    thrift::PrefixEntry entry;
    entry.prefix_ref() = toIpPrefix(network);
    entry.metrics_ref() = std::move(metrics);
    // ATTN: `area_stack` will be explicitly set to empty
    //      as there is no "cross-area" behavior for local
    //      originated prefixes.
    CHECK(entry.area_stack_ref()->empty());
    if (auto tags = prefix.tags_ref()) {
      entry.tags_ref() = *tags;
    }

    // Populate RibUnicastEntry struct
    // ATTN: AREA field is empty for NHs
    RibUnicastEntry unicastEntry(network, {createNextHop(toBinaryAddress(nh))});
    unicastEntry.bestPrefixEntry = std::move(entry);

    // ATTN: upon initialization, no supporting routes
    originatedPrefixDb_.emplace(
        network,
        OriginatedRoute(
            prefix,
            std::move(unicastEntry),
            std::unordered_set<folly::CIDRNetwork>{}));
  }
}

std::unordered_set<std::string>
PrefixManager::updateKvStorePrefixEntry(PrefixEntry const& entry) {
  std::unordered_set<std::string> prefixKeys;

  auto dstAreas = entry.dstAreas; // intended copy
  auto& prefixEntry = entry.tPrefixEntry;
  // prevent area_stack loop
  // ATTN: for local-originated prefixes, `area_stack` is explicitly
  //       set to empty.
  for (const auto fromArea : *prefixEntry.area_stack_ref()) {
    dstAreas.erase(fromArea);
  }

  for (const auto& toArea : dstAreas) {
    // TODO: run ingress policy
    auto [prefixKey, prefixDb] =
        createPrefixKeyAndDb(nodeId_, prefixEntry, toArea);

    if (enablePerfMeasurement_) {
      prefixDb.perfEvents_ref() =
          addingEvents_[*prefixEntry.type_ref()][*prefixEntry.prefix_ref()];
    }
    auto prefixDbStr = writeThriftObjStr(std::move(prefixDb), serializer_);

    bool changed = kvStoreClient_->persistKey(
        AreaId{toArea},
        prefixKey.getPrefixKey(),
        prefixDbStr,
        ttlKeyInKvStore_);
    fb303::fbData->addStatValue(
        "prefix_manager.route_advertisements", 1, fb303::SUM);
    VLOG_IF(1, changed) << "[ROUTE ADVERTISEMENT] "
                        << "Area: " << toArea << ", "
                        << "Type: " << toString(*prefixEntry.type_ref()) << ", "
                        << toString(prefixEntry, VLOG_IS_ON(2));
    prefixKeys.insert(prefixKey.getPrefixKey());
  }
  return prefixKeys;
}

void
PrefixManager::syncKvStore() {
  std::vector<std::pair<std::string, std::string>> keyVals;
  std::unordered_set<std::string> nowAdvertisingKeys;

  LOG(INFO) << "Syncing " << prefixMap_.size()
            << " route advertisements in KvStore";

  for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
    CHECK(not typeToPrefixes.empty()) << "Unexpected empty entry";
    auto bestType = *selectBestPrefixMetrics(typeToPrefixes).begin();
    auto& bestEntry = typeToPrefixes.at(bestType);
    addPerfEventIfNotExist(
        addingEvents_[bestType][prefix], "UPDATE_KVSTORE_THROTTLED");
    for (const auto& key : updateKvStorePrefixEntry(bestEntry)) {
      nowAdvertisingKeys.emplace(key);
      keysToClear_.erase(key);
    }
  }

  thrift::PrefixDatabase deletedPrefixDb;
  *deletedPrefixDb.thisNodeName_ref() = nodeId_;
  deletedPrefixDb.deletePrefix_ref() = true;
  if (enablePerfMeasurement_) {
    deletedPrefixDb.perfEvents_ref() = thrift::PerfEvents{};
    addPerfEventIfNotExist(
        deletedPrefixDb.perfEvents_ref().value(), "WITHDRAW_THROTTLED");
  }
  for (auto const& key : keysToClear_) {
    auto prefixKey = PrefixKey::fromStr(key);
    if (prefixKey.hasValue()) {
      // Needed for backward compatibility
      thrift::PrefixEntry entry;
      entry.prefix_ref() = prefixKey.value().getIpPrefix();
      *deletedPrefixDb.prefixEntries_ref() = {entry};
      VLOG(1) << "[ROUTE WITHDRAW] "
              << "Area: " << prefixKey->getPrefixArea() << ", "
              << toString(*entry.prefix_ref());
      fb303::fbData->addStatValue(
          "prefix_manager.route_withdraws", 1, fb303::SUM);
    } else {
      LOG(ERROR) << "[ROUTE WITHDRAW] Removing old key " << key
                 << " from KvStore";
    }

    // one last key set with empty DB and deletePrefix set signifies withdraw
    // then the key should ttl out
    kvStoreClient_->clearKey(
        AreaId{prefixKey->getPrefixArea()},
        key,
        writeThriftObjStr(std::move(deletedPrefixDb), serializer_),
        ttlKeyInKvStore_);
  }

  // anything we don't advertise next time, we need to clear
  keysToClear_ = std::move(nowAdvertisingKeys);

  // Update flat counters
  size_t num_prefixes = 0;
  for (auto const& kv : prefixMap_) {
    num_prefixes += kv.second.size();
  }
  fb303::fbData->setCounter("prefix_manager.received_prefixes", num_prefixes);
  fb303::fbData->setCounter(
      "prefix_manager.advertised_prefixes", prefixMap_.size());
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
    for (const auto& [_, typeToInfo] : prefixMap_) {
      for (const auto& [_, entry] : typeToInfo) {
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
    for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
      auto it = typeToPrefixes.find(prefixType);
      if (it != typeToPrefixes.end()) {
        prefixes.emplace_back(it->second.tPrefixEntry);
      }
    }
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
        std::move(prefixes)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
PrefixManager::getAdvertisedRoutesFiltered(
    thrift::AdvertisedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>();
  runInEventBaseThread(
      [this, p = std::move(p), filter = std::move(filter)]() mutable noexcept {
        auto routes =
            std::make_unique<std::vector<thrift::AdvertisedRouteDetail>>();
        if (filter.prefixes_ref()) {
          // Explicitly lookup the requested prefixes
          for (auto& prefix : filter.prefixes_ref().value()) {
            auto it = prefixMap_.find(prefix);
            if (it == prefixMap_.end()) {
              continue;
            }
            filterAndAddAdvertisedRoute(
                *routes, filter.prefixType_ref(), it->first, it->second);
          }
        } else {
          // Iterate over all prefixes
          for (auto& [prefix, prefixEntries] : prefixMap_) {
            filterAndAddAdvertisedRoute(
                *routes, filter.prefixType_ref(), prefix, prefixEntries);
          }
        }
        p.setValue(std::move(routes));
      });
  return std::move(sf);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::OriginatedPrefixEntry>>>
PrefixManager::getOriginatedPrefixes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::OriginatedPrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p)]() mutable noexcept {
    // convert content inside originatedPrefixDb_ into thrift struct
    auto prefixes =
        std::make_unique<std::vector<thrift::OriginatedPrefixEntry>>();
    for (auto const& [_, route] : originatedPrefixDb_) {
      auto const& prefix = route.originatedPrefix;
      auto supportingRoutes =
          folly::gen::from(route.supportingRoutes) |
          folly::gen::mapped([](const folly::CIDRNetwork& network) {
            return folly::IPAddress::networkToString(network);
          }) |
          folly::gen::as<std::vector<std::string>>();

      auto entry = createOriginatedPrefixEntry(
          prefix,
          supportingRoutes,
          supportingRoutes.size() >= *prefix.minimum_supporting_routes_ref());
      prefixes->emplace_back(std::move(entry));
    }
    p.setValue(std::move(prefixes));
  });
  return sf;
}

void
PrefixManager::filterAndAddAdvertisedRoute(
    std::vector<thrift::AdvertisedRouteDetail>& routes,
    apache::thrift::optional_field_ref<thrift::PrefixType&> const& typeFilter,
    thrift::IpPrefix const& prefix,
    std::unordered_map<thrift::PrefixType, PrefixEntry> const& prefixEntries) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::AdvertisedRouteDetail routeDetail;
  routeDetail.prefix_ref() = prefix;

  // Add best route selection data
  for (auto& prefixType : selectBestPrefixMetrics(prefixEntries)) {
    routeDetail.bestKeys_ref()->emplace_back(prefixType);
  }
  routeDetail.bestKey_ref() = routeDetail.bestKeys_ref()->at(0);

  // Add prefix entries and honor the filter
  for (auto& [prefixType, prefixEntry] : prefixEntries) {
    if (typeFilter && *typeFilter != prefixType) {
      continue;
    }
    routeDetail.routes_ref()->emplace_back();
    auto& route = routeDetail.routes_ref()->back();
    route.key_ref() = prefixType;
    route.route_ref() = prefixEntry.tPrefixEntry;
  }

  // Add detail if there are entries to return
  if (routeDetail.routes_ref()->size()) {
    routes.emplace_back(std::move(routeDetail));
  }
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

    auto& prefixes = prefixMap_[prefix];
    auto prefixIt = prefixes.find(type);

    // received same prefix entry, ignore
    if (prefixIt != prefixes.end() and prefixIt->second == entry) {
      continue;
    }

    if (prefixIt == prefixes.end()) {
      prefixes.emplace(type, entry);
      addPerfEventIfNotExist(addingEvents_[type][prefix], "ADD_PREFIX");
    } else {
      prefixIt->second = entry;
      addPerfEventIfNotExist(addingEvents_[type][prefix], "UPDATE_PREFIX");
    }
    updated = true;
  }

  if (updated) {
    syncKvStoreThrottled_->operator()();
  }

  return updated;
}

bool
PrefixManager::withdrawPrefixesImpl(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  // verify prefixes exists
  for (const auto& prefix : prefixes) {
    auto typeIt = prefixMap_.find(*prefix.prefix_ref());
    if (typeIt == prefixMap_.end()) {
      return false;
    }
    auto it = typeIt->second.find(*prefix.type_ref());
    if (it == typeIt->second.end()) {
      LOG(ERROR) << "Cannot withdraw non-existent prefix "
                 << toString(*prefix.prefix_ref())
                 << ", client: " << toString(*prefix.type_ref());
      return false;
    }
  }

  for (const auto& prefix : prefixes) {
    prefixMap_.at(*prefix.prefix_ref()).erase(*prefix.type_ref());
    addingEvents_.at(*prefix.type_ref()).erase(*prefix.prefix_ref());
    if (prefixMap_.at(*prefix.prefix_ref()).empty()) {
      prefixMap_.erase(*prefix.prefix_ref());
    }
    if (addingEvents_[*prefix.type_ref()].empty()) {
      addingEvents_.erase(*prefix.type_ref());
    }
  }

  if (!prefixes.empty()) {
    syncKvStoreThrottled_->operator()();
  }

  return !prefixes.empty();
}

bool
PrefixManager::syncPrefixesByTypeImpl(
    thrift::PrefixType type,
    const std::vector<thrift::PrefixEntry>& prefixEntries,
    const std::unordered_set<std::string>& dstAreas) {
  LOG(INFO) << "Syncing prefixes of type " << toString(type);
  // building these lists so we can call add and remove and get detailed logging
  std::vector<thrift::PrefixEntry> toAddOrUpdate, toRemove;
  std::unordered_set<thrift::IpPrefix> toRemoveSet;
  for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
    if (typeToPrefixes.count(type)) {
      toRemoveSet.emplace(prefix);
    }
  }
  for (auto const& entry : prefixEntries) {
    CHECK(type == *entry.type_ref());
    toRemoveSet.erase(*entry.prefix_ref());
    toAddOrUpdate.emplace_back(entry);
  }
  for (auto const& prefix : toRemoveSet) {
    toRemove.emplace_back(prefixMap_.at(prefix).at(type).tPrefixEntry);
  }
  bool updated = false;
  updated |= advertisePrefixesImpl(toAddOrUpdate, dstAreas);
  updated |= withdrawPrefixesImpl(toRemove);
  return updated;
}

bool
PrefixManager::withdrawPrefixesByTypeImpl(thrift::PrefixType type) {
  std::vector<thrift::PrefixEntry> toRemove;
  for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
    auto it = typeToPrefixes.find(type);
    if (it != typeToPrefixes.end()) {
      toRemove.emplace_back(it->second.tPrefixEntry);
    }
  }

  return withdrawPrefixesImpl(toRemove);
}

void
PrefixManager::aggregatesToAdvertise(const folly::CIDRNetwork& prefix) {
  // ATTN: ignore attribute-ONLY update for existing RIB entries
  //       as it won't affect `supporting_route_cnt`
  auto [ribPrefixIt, inserted] =
      ribPrefixDb_.emplace(prefix, std::vector<folly::CIDRNetwork>());
  if (not inserted) {
    return;
  }

  for (auto& [network, route] : originatedPrefixDb_) {
    // folly::CIDRNetwork.first -> IPAddress
    // folly::CIDRNetwork.second -> cidr length
    if (not prefix.first.inSubnet(network.first, network.second)) {
      continue;
    }

    VLOG(1) << "[ROUTE ORIGINATION] Adding supporting route "
            << folly::IPAddress::networkToString(prefix)
            << " for originated route "
            << folly::IPAddress::networkToString(network);

    // reverse mapping: RIB prefixEntry -> OriginatedPrefixes
    ribPrefixIt->second.emplace_back(network);

    // mapping: OriginatedPrefix -> RIB prefixEntries
    route.supportingRoutes.emplace(prefix);
  }
}

void
PrefixManager::aggregatesToWithdraw(const folly::CIDRNetwork& prefix) {
  // ignore invalid RIB entry
  auto ribPrefixIt = ribPrefixDb_.find(prefix);
  if (ribPrefixIt == ribPrefixDb_.end()) {
    return;
  }

  // clean mapping
  for (auto& network : ribPrefixIt->second) {
    auto originatedPrefixIt = originatedPrefixDb_.find(network);
    CHECK(originatedPrefixIt != originatedPrefixDb_.end());
    auto& route = originatedPrefixIt->second;

    VLOG(1) << "[ROUTE ORIGINATION] Removing supporting route "
            << folly::IPAddress::networkToString(prefix)
            << " for originated route "
            << folly::IPAddress::networkToString(network);

    route.supportingRoutes.erase(prefix);
  }

  // clean local caching
  ribPrefixDb_.erase(prefix);
}

void
PrefixManager::processDecisionRouteUpdates(
    DecisionRouteUpdate&& decisionRouteUpdate) {
  std::vector<PrefixEntry> advertisePrefixes{};
  std::vector<thrift::PrefixEntry> withdrawPrefixes{};
  thrift::RouteDatabaseDelta routeUpdates;

  // Add/Update unicast routes to update
  // Self originated (include routes imported from local BGP)
  // won't show up in decisionRouteUpdate.
  for (auto& [prefix, route] : decisionRouteUpdate.unicastRoutesToUpdate) {
    auto& prefixEntry = route.bestPrefixEntry;

    // NOTE: future expansion - run egress policy here

    //
    // cross area, modify attributes
    //

    // 1. append area stack
    prefixEntry.area_stack_ref()->emplace_back(route.bestArea);
    // 2. increase distance by 1
    ++(*prefixEntry.metrics_ref()->distance_ref());
    // 3. normalize to RIB routes
    prefixEntry.type_ref() = thrift::PrefixType::RIB;

    auto dstAreas = allAreas_;
    for (const auto& nh : route.nexthops) {
      if (nh.area_ref().has_value()) {
        dstAreas.erase(*nh.area_ref());
      }
    }
    advertisePrefixes.emplace_back(prefixEntry, dstAreas);

    // populate originated prefixes to be advertised
    aggregatesToAdvertise(prefix);
  }

  // Delete unicast routes
  for (const auto& prefix : decisionRouteUpdate.unicastRoutesToDelete) {
    withdrawPrefixes.emplace_back(
        createPrefixEntry(toIpPrefix(prefix), thrift::PrefixType::RIB));

    // populate originated prefixes to be withdrawn
    aggregatesToWithdraw(prefix);
  }

  for (auto& [network, route] : originatedPrefixDb_) {
    bool installToFib = route.originatedPrefix.install_to_fib_ref().has_value()
        ? *route.originatedPrefix.install_to_fib_ref()
        : true;
    if (not installToFib) {
      VLOG(2) << "Skip originated prefix: "
              << folly::IPAddress::networkToString(network)
              << " since route is marked as NO-installation";
      continue;
    }

    const auto& minSupportingCnt =
        *route.originatedPrefix.minimum_supporting_routes_ref();
    if ((not route.isAdvertised) and
        route.supportingRoutes.size() >= minSupportingCnt) {
      // skip processing if originatedPrefix is marked as `doNotInstall`
      route.isAdvertised = true; // mark as advertised
      routeUpdates.unicastRoutesToUpdate_ref()->emplace_back(
          route.unicastEntry.toThrift());

      SYSLOG(INFO) << "[ROUTE ORIGINATION] Advertising originated route "
                   << folly::IPAddress::networkToString(network);
    }

    if (route.isAdvertised and
        route.supportingRoutes.size() < minSupportingCnt) {
      route.isAdvertised = false; // mark as withdrawn
      routeUpdates.unicastRoutesToDelete_ref()->emplace_back(
          toIpPrefix(network));

      SYSLOG(INFO) << "[ROUTE ORIGINATION] Withdrawing originated route "
                   << folly::IPAddress::networkToString(network);
    }
  }

  // push originatedRoutes update via replicate queue
  if (routeUpdates.unicastRoutesToUpdate_ref()->size() or
      routeUpdates.unicastRoutesToDelete_ref()->size()) {
    CHECK(routeUpdates.mplsRoutesToUpdate_ref()->empty());
    CHECK(routeUpdates.mplsRoutesToDelete_ref()->empty());
    staticRouteUpdatesQueue_.push(std::move(routeUpdates));
  }

  // Redisrtibute RIB route ONLY when there are multiple `areaId` configured .
  // We want to keep processDecisionRouteUpdates() running as dynamic
  // configuration could add/remove areas.
  if (allAreas_.size() > 1) {
    advertisePrefixesImpl(advertisePrefixes);
    withdrawPrefixesImpl(withdrawPrefixes);
  }

  // ignore mpls updates
} // namespace openr

void
PrefixManager::addPerfEventIfNotExist(
    thrift::PerfEvents& perfEvents, std::string const& updateEvent) {
  if (perfEvents.events_ref()->empty() or
      *perfEvents.events_ref()->back().eventDescr_ref() != updateEvent) {
    addPerfEvent(perfEvents, nodeId_, updateEvent);
  }
}

} // namespace openr
