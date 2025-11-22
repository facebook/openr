/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/debugging/exception_tracer/ExceptionTracer.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/prefix-manager/PrefixManager.h>

namespace openr {

namespace fb303 = facebook::fb303;

PrefixManager::PrefixManager(
    messaging::ReplicateQueue<DecisionRouteUpdate>& staticRouteUpdatesQueue,
    messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue,
    messaging::ReplicateQueue<thrift::InitializationEvent>&
        initializationEventQueue,
    messaging::RQueue<KvStorePublication> kvStoreUpdatesQueue,
    messaging::RQueue<PrefixEvent> prefixUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> fibRouteUpdatesQueue,
    std::shared_ptr<const Config> config)
    : nodeId_(config->getNodeName()),
      config_(config),
      staticRouteUpdatesQueue_(staticRouteUpdatesQueue),
      kvRequestQueue_(kvRequestQueue),
      initializationEventQueue_(initializationEventQueue) {
  CHECK(config);

  // Always add RIB type prefixes, since Fib routes updates are always expected
  // in OpenR initialization procedure.
  XLOG(INFO) << "[Initialization] PrefixManager should wait for RIB updates.";
  uninitializedPrefixTypes_.emplace(thrift::PrefixType::RIB);

  if (config->isVipServiceEnabled()) {
    XLOG(INFO)
        << "[Initialization] PrefixManager should wait for VIP prefixes.";
    uninitializedPrefixTypes_.emplace(thrift::PrefixType::VIP);
  }

  if (config->getConfig().originated_prefixes()) {
    XLOG(INFO)
        << "[Initialization] PrefixManager should wait for CONFIG prefixes.";
    uninitializedPrefixTypes_.emplace(thrift::PrefixType::CONFIG);
  }

  if (auto policyConf = config->getAreaPolicies()) {
    policyManager_ = std::make_unique<PolicyManager>(*policyConf);
  }

  for (const auto& [areaId, areaConf] : config->getAreas()) {
    areaToPolicy_.emplace(areaId, areaConf.getImportPolicyName());
  }

  initCounters();

  // Create throttled update state
  syncKvStoreThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kKvStoreSyncThrottleTimeout, [this]() noexcept {
        /*
         * [Initialization]
         *
         * With signal-based initialization sequence, all PREFIX_TYPES must be
         * processed/received before writing KvStore.
         *
         * Otherwise, it will bail out.
         *
         * Attention: RIB type updates indicates the following events
         * accomplished:
         *  - KVSTORE_SYNCED published by KvStore
         *  - RIB_COMPUTED published by Decision
         *  - FIB_SYNCED published by Fib
         *
         * PrefixManager received first RIB type updates from Fib module to mark
         * ready to write to KvStore.
         */
        if (uninitializedPrefixTypes_.empty()) {
          syncKvStore();
        }
      });

  // Load openrConfig for local-originated routes
  addFiberTask([this]() mutable noexcept {
    if (auto prefixes = config_->getConfig().originated_prefixes()) {
      // Build originated prefixes from OpenrConfig.
      buildOriginatedPrefixes(*prefixes);

      // Trigger initial prefix sync in KvStore.
      XLOG(INFO) << "[Initialization] Processed CONFIG type prefixes.";
      uninitializedPrefixTypes_.erase(thrift::PrefixType::CONFIG);
      triggerInitialPrefixDbSync();
    }
  });

  // Schedule fiber to read prefix updates messages
  addFiberTask([q = std::move(prefixUpdatesQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting prefix-updates processing task";
    while (true) {
      auto maybeUpdate = q.get(); // perform read
      if (maybeUpdate.hasError()) {
        break;
      }
      auto& update = maybeUpdate.value();

      // if no specified dstination areas, apply to all areas
      std::unordered_set<std::string> dstAreas;
      if (update.dstAreas.empty()) {
        dstAreas = allAreaIds();
      } else {
        for (const auto& area : update.dstAreas) {
          dstAreas.emplace(area);
        }
      }

      switch (update.eventType) {
      case PrefixEventType::ADD_PREFIXES: {
        XLOGF(
            DBG1,
            "[Prefix Event] Announcing {} prefixes to areas: {}",
            update.prefixes.size() + update.prefixEntries.size(),
            folly::join(",", dstAreas));
        advertisePrefixesImpl(std::move(update.prefixes), dstAreas);
        advertisePrefixesImpl(
            std::move(std::move(update.prefixEntries)),
            dstAreas,
            update.policyName);

        if (uninitializedPrefixTypes_.erase(update.type)) {
          // Received initial prefixes of certain type during initialization
          XLOG(INFO) << fmt::format(
              "[Initialization] Received {} prefixes of type {}.",
              update.prefixes.size() + update.prefixEntries.size(),
              apache::thrift::util::enumNameSafe<thrift::PrefixType>(
                  update.type));
          // Publish initial unicast routes for the prefix type, so they will be
          // programmed in warmboot.
          sendStaticUnicastRoutes(update.type);

          triggerInitialPrefixDbSync();
        }
        break;
      }
      case PrefixEventType::WITHDRAW_PREFIXES:
        XLOGF(
            DBG1,
            "[Prefix Event] Withdrawing {} prefixes from areas: {}",
            update.prefixes.size() + update.prefixEntries.size(),
            folly::join(",", dstAreas));
        withdrawPrefixesImpl(update.prefixes);
        withdrawPrefixEntriesImpl(update.prefixEntries);
        break;
      case PrefixEventType::WITHDRAW_PREFIXES_BY_TYPE:
        XLOGF(
            DBG1,
            "[Prefix Event] Withdrawing all prefix with type {} from all areas",
            apache::thrift::util::enumNameSafe(update.type));
        withdrawPrefixesByTypeImpl(update.type);
        break;
      case PrefixEventType::SYNC_PREFIXES_BY_TYPE:
        syncPrefixesByTypeImpl(
            update.type, update.prefixes, dstAreas, update.policyName);
        break;
      default:
        XLOG(ERR) << "Unknown command received. "
                  << static_cast<int>(update.eventType);
      }
    }
    XLOG(DBG1) << "[Exit] Prefix-updates processing task finished.";
  });

  // Fiber to process route updates from Fib.
  addFiberTask([q = std::move(fibRouteUpdatesQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting fib route-update processing task";
    while (true) {
      auto maybeThriftObj = q.get(); // perform read
      if (maybeThriftObj.hasError()) {
        break;
      }

      try {
        processFibRouteUpdates(std::move(maybeThriftObj).value());
      } catch (const std::exception&) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
        // collect stack strace then fail the process
        for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
          XLOG(ERR) << exInfo;
        }
#endif
        throw;
      }
    }
    XLOG(DBG1) << "[Exit] Fib route-updates processing task finished.";
  });

  // Fiber to process publication from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    XLOG(DBG1) << "Starting kvStore-updates processing task";
    while (true) {
      auto maybePub = q.get(); // perform read
      if (maybePub.hasError()) {
        break;
      }
      if (maybePub.hasValue()) {
        // process different types of event
        folly::variant_match(
            std::move(maybePub).value(),
            [this](thrift::Publication&& pub) {
              // Process KvStore Thrift publication.
              processPublication(std::move(pub));
            },
            [](thrift::InitializationEvent&& /* unused */) { return; });
      }
    }
    XLOG(DBG1) << "[Exit] KvStore-updates processing task finished.";
  });
}

void
PrefixManager::processPublication(thrift::Publication&& thriftPub) {
  folly::small_vector<folly::CIDRNetwork> changed{};
  for (const auto& [keyStr, val] : *thriftPub.keyVals()) {
    // Only interested in prefix updates.
    // Ignore if val has no value field inside `thrift::Value`(e.g. ttl update)
    auto const& area = *thriftPub.area();
    try {
      const auto prefixDb =
          readThriftObjStr<thrift::PrefixDatabase>(*val.value(), serializer_);
      if (prefixDb.prefixEntries()->size() != 1) {
        LOG(WARNING) << "Skip processing unexpected number of prefix entries";
        continue;
      }

      if (!*prefixDb.deletePrefix()) {
        // get the key prefix and area from the thrift::PrefixDatabase
        auto const& tPrefixEntry = prefixDb.prefixEntries()->front();
        auto const& thisNodeName = *prefixDb.thisNodeName();
        auto const& network = toIPNetwork(*tPrefixEntry.prefix());

        // Skip none-self advertised prefixes or already persisted keys.
        if (thisNodeName != nodeId_ || advertiseStatus_.count(network) > 0) {
          continue;
        }

        XLOG(DBG1) << fmt::format(
            "[Prefix Update]: Area: {}, {} updated inside KvStore",
            area,
            keyStr);
        // populate advertiseStatus_ collection to make sure we can find
        // <key, area> when clear key from `KvStore`
        advertiseStatus_[network].areas.emplace(area);

        // Populate pendingState to check keys
        pendingUpdates_.addPrefixChange(network);
        syncKvStoreThrottled_->operator()();
      }
    } catch (const std::exception& ex) {
      XLOG(ERR) << "Failed to deserialize corresponding value for key "
                << keyStr << ". Exception: " << folly::exceptionStr(ex);
    }
  } // for
}

PrefixManager::~PrefixManager() {
  XLOG(DBG1) << fmt::format(
      "[Exit] Send termination signals to stop {} tasks.", getFiberTaskNum());

  // - If EventBase is stopped or it is within the evb thread, run immediately;
  // - Otherwise, will wait the EventBase to run;
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this]() { syncKvStoreThrottled_.reset(); });

  // Invoke stop method of super class
  OpenrEventBase::stop();
  XLOG(DBG1) << "[Exit] Succeessfully stopped PrefixManager eventbase.";
}

thrift::PrefixEntry
PrefixManager::toPrefixEntryThrift(
    const thrift::OriginatedPrefix& prefix, const thrift::PrefixType& tType) {
  // Populate PrefixMetric struct
  thrift::PrefixMetrics metrics;
  if (auto pref = prefix.path_preference()) {
    metrics.path_preference() = *pref;
  }
  if (auto pref = prefix.source_preference()) {
    metrics.source_preference() = *pref;
  }

  // Populate PrefixEntry struct
  thrift::PrefixEntry entry;
  entry.prefix() =
      toIpPrefix(folly::IPAddress::createNetwork(*prefix.prefix()));
  entry.metrics() = std::move(metrics);
  // ATTN: local-originated prefix has unique type CONFIG
  //      to be differentiated from others.
  entry.type() = tType;
  // ATTN: `area_stack` will be explicitly set to empty
  //      as there is no "cross-area" behavior for local
  //      originated prefixes.
  CHECK(entry.area_stack()->empty());
  if (auto tags = prefix.tags()) {
    entry.tags() = *tags;
  }
  return entry;
}

void
PrefixManager::buildOriginatedPrefixes(
    const std::vector<thrift::OriginatedPrefix>& prefixes) {
  DecisionRouteUpdate routeUpdatesForDecision;
  routeUpdatesForDecision.prefixType = thrift::PrefixType::CONFIG;

  for (const auto& prefix : prefixes) {
    auto network = folly::IPAddress::createNetwork(*prefix.prefix());
    auto entry = toPrefixEntryThrift(prefix, thrift::PrefixType::CONFIG);

    // Populate RibUnicastEntry struct
    // ATTN: empty nexthop list indicates a drop route
    RibUnicastEntry unicastEntry(network, {});
    unicastEntry.bestPrefixEntry = std::move(entry);

    if (prefix.install_to_fib().has_value() && *prefix.install_to_fib()) {
      routeUpdatesForDecision.addRouteToUpdate(unicastEntry);
      advertiseStatus_[network].publishedRoute = unicastEntry;
    }

    // ATTN: upon initialization, no supporting routes
    originatedPrefixDb_.emplace(
        network,
        OriginatedRoute(
            prefix,
            std::move(unicastEntry),
            std::unordered_set<folly::CIDRNetwork>{}));
  }
  fb303::fbData->addStatValue(
      "prefix_manager.originated_routes",
      originatedPrefixDb_.size(),
      fb303::SUM);

  // Publish static routes for config originated prefixes. This makes sure
  // initial RIB/FIB after warmboot still includes routes for config originated
  // prefixes.
  staticRouteUpdatesQueue_.push(std::move(routeUpdatesForDecision));
}

std::pair<thrift::PrefixType, const PrefixEntry>
PrefixManager::getBestPrefixEntry(
    const std::unordered_map<thrift::PrefixType, PrefixEntry>&
        prefixTypeToEntry) {
  // If decision calculation has already considered local routes, then we should
  // use the best entry provided by decision instead of calculating here again.
  const auto it = prefixTypeToEntry.find(thrift::PrefixType::RIB);
  if (prefixTypeToEntry.end() != it && it->second.preferredForRedistribution) {
    return std::make_pair(thrift::PrefixType::RIB, it->second);
  }
  // select the best entry/entries by comparing metric field
  const auto bestTypes = selectBestPrefixMetrics(prefixTypeToEntry);
  auto bestType = *bestTypes.begin();
  return std::make_pair(bestType, prefixTypeToEntry.at(bestType));
}

void
PrefixManager::sendStaticUnicastRoutes(thrift::PrefixType prefixType) {
  DecisionRouteUpdate routeUpdatesForDecision;
  routeUpdatesForDecision.prefixType = prefixType;

  /*
   * During initialization, when PrefixManager receives prefixes of a particular
   * type, this function is used to send prefixEntries corresponding that type
   * to Decision.
   */
  for (const auto& [prefix, prefixEntries] : prefixMap_) {
    // Only populate the best entry
    auto [bestType, bestEntry] = getBestPrefixEntry(prefixEntries);
    if (bestType != prefixType) {
      continue;
    }
    populateRouteUpdates(prefix, bestEntry, routeUpdatesForDecision);
  }
  staticRouteUpdatesQueue_.push(std::move(routeUpdatesForDecision));
}

void
PrefixManager::populateRouteUpdates(
    const folly::CIDRNetwork& prefix,
    const PrefixEntry& prefixEntry,
    DecisionRouteUpdate& routeUpdatesForDecision) {
  // Propogate route update to Decision (if necessary)
  auto& advertiseStatus = advertiseStatus_[prefix];
  if (prefixEntry.shouldInstall()) {
    // Populate RibUnicastEntry struct
    // ATTN: AREA field is empty for NHs
    // if shouldInstall() is true, nexthops is guaranteed to have value.
    RibUnicastEntry unicastEntry(prefix, prefixEntry.nexthops.value());
    unicastEntry.bestPrefixEntry = *prefixEntry.tPrefixEntry;
    if (advertiseStatus.publishedRoute.has_value() &&
        advertiseStatus.publishedRoute.value() == unicastEntry) {
      // Avoid publishing duplicate routes for the prefix.
      return;
    }

    advertiseStatus.publishedRoute = unicastEntry;
    routeUpdatesForDecision.addRouteToUpdate(std::move(unicastEntry));
  } else {
    // If was installed to fib, but now lose in tie break, withdraw from fib.
    if (advertiseStatus.publishedRoute.has_value()) {
      routeUpdatesForDecision.unicastRoutesToDelete.emplace_back(prefix);
      advertiseStatus.publishedRoute.reset();
    }
  } // else
}

void
PrefixManager::updatePrefixKeysInKvStore(
    const folly::CIDRNetwork& prefix, const PrefixEntry& prefixEntry) {
  // advertise best-entry for this prefix to `KvStore`
  auto updatedArea = addKvStoreKeyHelper(prefixEntry);

  auto keysIt = advertiseStatus_.find(prefix);
  if (keysIt != advertiseStatus_.end()) {
    // ATTN: advertiseStatus_ collection holds "advertised" prefixes in
    // previous round of syncing. By removing prefixes in current run,
    // whatever left in `advertiseStatus_` will be the delta to be removed.
    for (const auto& area : updatedArea) {
      keysIt->second.areas.erase(area);
    }

    // remove keys which are no longer advertised
    // e.g.
    // t0: prefix_1 => {area_1, area_2}
    // t1: prefix_1 => {area_1, area_3}
    //     (prefix_1, area_2) will be removed
    deleteKvStoreKeyHelper(prefix, keysIt->second.areas);
  }

  // override `advertiseStatus_` for next-round syncing
  advertiseStatus_[prefix].areas = std::move(updatedArea);
  advertiseStatus_[prefix].advertisedBestEntry = prefixEntry;
}

std::unordered_set<std::string>
PrefixManager::addKvStoreKeyHelper(const PrefixEntry& entry) {
  std::unordered_set<std::string> areasToUpdate;
  const auto& tPrefixEntry = entry.tPrefixEntry;
  const auto& type = *tPrefixEntry->type();
  const std::unordered_set<std::string> areaStack{
      tPrefixEntry->area_stack()->begin(), tPrefixEntry->area_stack()->end()};

  for (const auto& toArea : entry.dstAreas) {
    // prevent area_stack loop
    // ATTN: for local-originated prefixes, `area_stack` is explicitly
    //       set to empty.
    if (areaStack.count(toArea)) {
      continue;
    }

    // run ingress policy
    std::shared_ptr<thrift::PrefixEntry> postPolicyTPrefixEntry;
    std::string hitPolicyName;

    const auto& policy = areaToPolicy_.at(toArea);
    if (policy) {
      std::tie(postPolicyTPrefixEntry, hitPolicyName) =
          policyManager_->applyPolicy(
              *policy,
              tPrefixEntry,
              entry.policyActionData,
              entry.policyMatchData);

      // policy reject prefix, nothing to do.
      if (!postPolicyTPrefixEntry) {
        XLOG(DBG2) << "[Area Policy] " << *policy
                   << " rejected prefix: " << "(Type, PrefixEntry): ("
                   << toString(type) << ", " << toString(*tPrefixEntry, true)
                   << "), hit term (" << hitPolicyName << ")";
        fb303::fbData->addStatValue("prefix_manager.rejected", 1, fb303::SUM);
        fb303::fbData->addStatValue(
            fmt::format("prefix_manager.rejected.{}", toArea), 1, fb303::SUM);
        continue;
      }

      // policy accept prefix, go ahread with prefix announcement.
      XLOG(DBG2) << "[Area Policy] " << *policy
                 << " accepted/modified prefix: " << "(Type, PrefixEntry): ("
                 << toString(type) << ", " << toString(*tPrefixEntry, true)
                 << "), PostPolicyEntry: (" << toString(*postPolicyTPrefixEntry)
                 << "), hit term (" << hitPolicyName << ")";
    } else {
      postPolicyTPrefixEntry = tPrefixEntry;
    }

    const auto prefixKeyStr =
        PrefixKey(nodeId_, entry.network, toArea).getPrefixKeyV2();
    auto prefixDb = createPrefixDb(nodeId_, {*postPolicyTPrefixEntry});
    auto prefixDbStr = writeThriftObjStr(std::move(prefixDb), serializer_);

    // advertise key to `KvStore`
    auto persistPrefixKeyVal =
        PersistKeyValueRequest(AreaId{toArea}, prefixKeyStr, prefixDbStr);
    kvRequestQueue_.push(std::move(persistPrefixKeyVal));

    fb303::fbData->addStatValue(
        "prefix_manager.route_advertisements", 1, fb303::SUM);
    fb303::fbData->addStatValue(
        fmt::format("prefix_manager.route_advertisements.{}", toArea),
        1,
        fb303::SUM);
    XLOG(DBG1) << "[Prefix Advertisement] " << "Area: " << toArea << ", "
               << "Type: " << toString(type) << ", "
               << toString(*postPolicyTPrefixEntry->prefix())
               << toString(*postPolicyTPrefixEntry, VLOG_IS_ON(2));
    areasToUpdate.emplace(toArea);
  }
  return areasToUpdate;
}

void
PrefixManager::deletePrefixKeysInKvStore(
    const folly::CIDRNetwork& prefix,
    DecisionRouteUpdate& routeUpdatesForDecision) {
  auto prefixIter = advertiseStatus_.find(prefix);
  if (prefixIter != advertiseStatus_.end()) {
    deleteKvStoreKeyHelper(prefix, prefixIter->second.areas);
    if (prefixIter->second.publishedRoute.has_value()) {
      routeUpdatesForDecision.unicastRoutesToDelete.emplace_back(prefix);
    }
    advertiseStatus_.erase(prefixIter);
  }
}

void
PrefixManager::deleteKvStoreKeyHelper(
    const folly::CIDRNetwork& prefix,
    const std::unordered_set<std::string>& deletedArea) {
  for (const auto& area : deletedArea) {
    // Prepare thrift::PrefixDatabase object for deletion
    thrift::PrefixDatabase deletedPrefixDb;
    deletedPrefixDb.thisNodeName() = nodeId_;
    deletedPrefixDb.deletePrefix() = true;

    /*
     * delete actual keys being advertised in the cache
     *
     * Sample format:
     * prefix    :    node1    :    0.0.0.0/32
     *   |              |               |
     * marker         nodeId         prefixStr
     */
    const auto prefixKeyStr = PrefixKey(nodeId_, prefix, area).getPrefixKeyV2();
    thrift::PrefixEntry entry;
    entry.prefix() = toIpPrefix(prefix);
    deletedPrefixDb.prefixEntries() = {entry};

    // Remove prefix from KvStore and flood deletion by setting deleted value.
    auto unsetPrefixRequest = ClearKeyValueRequest(
        AreaId{area},
        prefixKeyStr,
        writeThriftObjStr(std::move(deletedPrefixDb), serializer_),
        true);
    kvRequestQueue_.push(std::move(unsetPrefixRequest));

    XLOG(DBG1) << "[Prefix Withdraw] " << "Area: " << area << ", "
               << toString(*entry.prefix());
    fb303::fbData->addStatValue(
        "prefix_manager.route_withdraws", 1, fb303::SUM);
  }
}

void
PrefixManager::triggerInitialPrefixDbSync() {
  if (uninitializedPrefixTypes_.empty()) {
    // Trigger initial syncKvStore(), after receiving prefixes of all expected
    // types and all inital prefix keys from KvStore.
    syncKvStore();

    // Logging for initialization stage duration computation
    logInitializationEvent(
        "PrefixManager", thrift::InitializationEvent::PREFIX_DB_SYNCED);

    // Notify downstream for initialization event
    initializationEventQueue_.push(
        thrift::InitializationEvent::PREFIX_DB_SYNCED);
  }
}

bool
PrefixManager::prefixEntryReadyToBeAdvertised(const PrefixEntry& prefixEntry) {
  // If nexthops is set in prefixEntry, it implicitly indicates that the
  // associated unicast routes should be programmed before the prefix is
  // advertised.
  if (prefixEntry.nexthops.has_value()) {
    if (programmedPrefixes_.count(prefixEntry.network) == 0) {
      return false;
    }
  }

  return true;
}

void
PrefixManager::syncKvStore() {
  XLOG(DBG1) << "[KvStore Sync] Syncing " << pendingUpdates_.size()
             << " pending updates.";

  DecisionRouteUpdate routeUpdatesForDecision;
  size_t receivedPrefixCnt = 0;
  size_t syncedPrefixCnt = 0;
  size_t awaitingPrefixCnt = 0;

  // Withdraw prefixes that no longer exist.
  for (auto const& prefix : pendingUpdates_.getChangedPrefixes()) {
    auto it = prefixMap_.find(prefix);
    if (it == prefixMap_.end()) {
      XLOG(DBG1) << fmt::format(
          "Deleting key: {} since it has been withdrawn.",
          folly::IPAddress::networkToString(prefix));

      deletePrefixKeysInKvStore(prefix, routeUpdatesForDecision);
      ++syncedPrefixCnt;
    }
  }

  // TODO: iterate the whole prefixMap_ is time consuming.
  // Explore scale enhancement
  for (const auto& [prefix, prefixEntries] : prefixMap_) {
    receivedPrefixCnt += prefixEntries.size();

    /*
     * Find the best entry out of a collection of prefix entries.
     */
    auto [_, bestEntry] = getBestPrefixEntry(prefixEntries);
    bool hasPrefixUpdate = pendingUpdates_.hasPrefix(prefix);
    bool readyToBeAdvertised = prefixEntryReadyToBeAdvertised(bestEntry);

    // Get route updates from updated prefix entry.
    if (hasPrefixUpdate) {
      populateRouteUpdates(prefix, bestEntry, routeUpdatesForDecision);
    }

    if (readyToBeAdvertised) {
      if (hasPrefixUpdate) {
        XLOG(DBG1) << fmt::format(
            "Adding/updating key: {} with best entry: {} to area: {}",
            folly::IPAddress::networkToString(prefix),
            toString(*bestEntry.tPrefixEntry, true),
            folly::join(",", bestEntry.dstAreas));

        updatePrefixKeysInKvStore(prefix, bestEntry);
        ++syncedPrefixCnt;
      }
    } else {
      // The prefix is awaiting to be advertised.
      ++awaitingPrefixCnt;

      XLOG(DBG1) << fmt::format(
          "Skip advertising key: {} since it is not ready to be advertised",
          folly::IPAddress::networkToString(prefix));

      /*
       * `advertiseStatus_` holds the "old" prefix entry advertised to
       * KvStore. Since it is no longer ready to be advertised, withdraw it
       * from KvStore.
       */
      const auto& it = advertiseStatus_.find(prefix);
      if (advertiseStatus_.cend() != it &&
          (!prefixEntryReadyToBeAdvertised(it->second.advertisedBestEntry))) {
        XLOG(DBG1) << fmt::format(
            "Deleting previously advertised key: {} from area: {}",
            folly::IPAddress::networkToString(prefix),
            folly::join(",", it->second.areas));

        deletePrefixKeysInKvStore(prefix, routeUpdatesForDecision);
        ++syncedPrefixCnt;
      }
    }
  } // for

  // Reset pendingUpdates_ since all pending updates are processed.
  pendingUpdates_.clear();

  // Push originatedRoutes update to staticRouteUpdatesQueue_.
  if (!routeUpdatesForDecision.empty()) {
    staticRouteUpdatesQueue_.push(std::move(routeUpdatesForDecision));
  }

  XLOG(DBG1) << fmt::format(
      "[KvStore Sync] Updated {} prefixes in KvStore; {} more awaiting FIB-ACK.",
      syncedPrefixCnt,
      awaitingPrefixCnt);

  // Update flat counters
  fb303::fbData->setCounter(
      "prefix_manager.received_prefixes", receivedPrefixCnt);
  // TODO: report per-area advertised prefixes if openr is running in
  // multi-areas.
  fb303::fbData->setCounter(
      "prefix_manager.advertised_prefixes", advertiseStatus_.size());
  fb303::fbData->setCounter(
      "prefix_manager.awaiting_prefixes", awaitingPrefixCnt);
}

folly::SemiFuture<bool>
PrefixManager::advertisePrefixes(std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixes = std::move(prefixes)]() mutable noexcept {
    auto dstAreas = allAreaIds();
    p.setValue(advertisePrefixesImpl(std::move(prefixes), dstAreas));
  });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::withdrawPrefixes(std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixes = std::move(prefixes)]() mutable noexcept {
    p.setValue(withdrawPrefixesImpl(prefixes));
  });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::withdrawPrefixesByType(thrift::PrefixType prefixType) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixType = std::move(prefixType)]() mutable noexcept {
    p.setValue(withdrawPrefixesByTypeImpl(prefixType));
  });
  return sf;
}

folly::SemiFuture<bool>
PrefixManager::syncPrefixesByType(
    thrift::PrefixType prefixType, std::vector<thrift::PrefixEntry> prefixes) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixType = std::move(prefixType),
                        prefixes = std::move(prefixes)]() mutable noexcept {
    auto dstAreas = allAreaIds();
    p.setValue(syncPrefixesByTypeImpl(prefixType, prefixes, dstAreas));
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
        prefixes.emplace_back(*entry.tPrefixEntry);
      }
    }
    p.setValue(
        std::make_unique<std::vector<thrift::PrefixEntry>>(
            std::move(prefixes)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
PrefixManager::getPrefixesByType(thrift::PrefixType prefixType) {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        prefixType = std::move(prefixType)]() mutable noexcept {
    std::vector<thrift::PrefixEntry> prefixes;
    for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
      auto it = typeToPrefixes.find(prefixType);
      if (it != typeToPrefixes.end()) {
        prefixes.emplace_back(*it->second.tPrefixEntry);
      }
    }
    p.setValue(
        std::make_unique<std::vector<thrift::PrefixEntry>>(
            std::move(prefixes)));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
PrefixManager::getAdvertisedRoutesFiltered(
    thrift::AdvertisedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>();
  runInEventBaseThread([this,
                        promise = std::move(p),
                        filter = std::move(filter)]() mutable noexcept {
    auto routes =
        std::make_unique<std::vector<thrift::AdvertisedRouteDetail>>();
    if (filter.prefixes()) {
      // Explicitly lookup the requested prefixes
      for (auto& prefix : filter.prefixes().value()) {
        auto it = prefixMap_.find(toIPNetwork(prefix));
        if (it == prefixMap_.end()) {
          continue;
        }
        filterAndAddAdvertisedRoute(
            *routes, filter.prefixType(), it->first, it->second);
      }
    } else {
      // Iterate over all prefixes
      for (auto& [prefix, prefixEntries] : prefixMap_) {
        filterAndAddAdvertisedRoute(
            *routes, filter.prefixType(), prefix, prefixEntries);
      }
    }
    promise.setValue(std::move(routes));
  });
  return std::move(sf);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>
PrefixManager::getAdvertisedRoutesWithOriginationPolicy(
    thrift::RouteFilterType routeFilterType,
    thrift::AdvertisedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>();
  runInEventBaseThread([this,
                        promise = std::move(p),
                        filter = std::move(filter),
                        routeFilterType = routeFilterType]() mutable noexcept {
    auto routes = std::make_unique<std::vector<thrift::AdvertisedRoute>>();
    if (filter.prefixes()) {
      // Explicitly lookup the requested prefixes
      for (auto& prefix : filter.prefixes().value()) {
        auto it = originatedPrefixMap_.find(toIPNetwork(prefix));
        if (it == originatedPrefixMap_.end()) {
          continue;
        }
        filterAndAddOriginatedRoute(
            *routes, routeFilterType, it->second, filter.prefixType());
      }
    } else {
      for (auto& [prefix, prefixEntries] : originatedPrefixMap_) {
        filterAndAddOriginatedRoute(
            *routes, routeFilterType, prefixEntries, filter.prefixType());
      }
    }
    promise.setValue(std::move(routes));
  });
  return std::move(sf);
}

void
PrefixManager::removeOriginatedPrefixes(
    const std::vector<thrift::PrefixEntry>& prefixEntries) {
  // Remove entry from originatedPrefixMap, regardless of policy
  for (const auto& entry : prefixEntries) {
    const auto& prefixCidr = toIPNetwork(*entry.prefix());
    auto typeIt = originatedPrefixMap_.find(prefixCidr);
    if (typeIt == originatedPrefixMap_.end()) {
      continue;
    }

    const auto& type = *entry.type();
    typeIt->second.erase(type);
    // clean up data structure
    if (typeIt->second.empty()) {
      originatedPrefixMap_.erase(prefixCidr);
    }
  }
}

void
PrefixManager::removeOriginatedPrefixes(
    const std::vector<PrefixEntry>& prefixEntries) {
  // Remove entry from originatedPrefixMap, regardless of policy
  for (const auto& entry : prefixEntries) {
    const auto& prefixCidr = entry.network;
    auto typeIt = originatedPrefixMap_.find(prefixCidr);
    if (typeIt == originatedPrefixMap_.end()) {
      continue;
    }
    const auto& type = *entry.tPrefixEntry->type();
    typeIt->second.erase(type);
    // clean up data structure
    if (typeIt->second.empty()) {
      originatedPrefixMap_.erase(prefixCidr);
    }
  }
}

void
PrefixManager::storeOriginatedPrefixes(
    std::vector<PrefixEntry> prefixEntries, const std::string& policyName) {
  // store prefixes that came with origination policy
  for (auto& entry : prefixEntries) {
    const auto& type = *entry.tPrefixEntry->type();
    const auto& prefixCidr = entry.network;
    originatedPrefixMap_[prefixCidr].insert_or_assign(
        type, std::make_pair(std::move(entry), policyName));
  }
}

void
PrefixManager::filterAndAddOriginatedRoute(
    std::vector<thrift::AdvertisedRoute>& routes,
    const thrift::RouteFilterType& routeFilterType,
    std::unordered_map<
        thrift::PrefixType,
        std::pair<PrefixEntry, std::string>> const& prefixEntries,
    apache::thrift::optional_field_ref<thrift::PrefixType&> const& typeFilter) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  // Loop entire map since we need to dump prefixes from all prefix type
  for (auto& [prefixType, prefixEntryPolicyPair] : prefixEntries) {
    if (typeFilter && *typeFilter != prefixType) {
      continue;
    }

    auto [prePolicyPrefixEntry, policy] = prefixEntryPolicyPair;
    const auto& prePolicyTPrefixEntry = prePolicyPrefixEntry.tPrefixEntry;

    // prefilter advertised route
    if (routeFilterType == thrift::RouteFilterType::PREFILTER_ADVERTISED) {
      thrift::AdvertisedRoute route;
      route.key() = *prePolicyTPrefixEntry->type();
      route.route() = *prePolicyTPrefixEntry;
      routes.emplace_back(std::move(route));
      continue;
    }

    auto [postPolicyTPrefixEntry, hitPolicyName] = policyManager_->applyPolicy(
        policy,
        prePolicyTPrefixEntry,
        std::nullopt /* policy Action Data */,
        prePolicyPrefixEntry.policyMatchData);
    if (routeFilterType == thrift::RouteFilterType::POSTFILTER_ADVERTISED &&
        postPolicyTPrefixEntry) {
      // add post filter advertised route
      thrift::AdvertisedRoute route;
      route.key() = *prePolicyTPrefixEntry->type();
      route.route() = *postPolicyTPrefixEntry;
      if (!hitPolicyName.empty()) {
        route.hitPolicy() = hitPolicyName;
      }
      routes.emplace_back(std::move(route));
      continue;
    }

    if (routeFilterType == thrift::RouteFilterType::REJECTED_ON_ADVERTISE &&
        !postPolicyTPrefixEntry) {
      // add post filter rejected route
      thrift::AdvertisedRoute route;
      route.key() = *prePolicyTPrefixEntry->type();
      route.route() = *prePolicyTPrefixEntry;
      route.hitPolicy() = hitPolicyName;
      routes.emplace_back(std::move(route));
    }
  }
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>
PrefixManager::getAreaAdvertisedRoutes(
    std::string areaName,
    thrift::RouteFilterType routeFilterType,
    thrift::AdvertisedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>();
  runInEventBaseThread([this,
                        promise = std::move(p),
                        filter = std::move(filter),
                        areaName = std::move(areaName),
                        routeFilterType = routeFilterType]() mutable noexcept {
    auto routes = std::make_unique<std::vector<thrift::AdvertisedRoute>>();
    if (filter.prefixes()) {
      // Explicitly lookup the requested prefixes
      for (auto& prefix : filter.prefixes().value()) {
        auto it = prefixMap_.find(toIPNetwork(prefix));
        if (it == prefixMap_.end()) {
          continue;
        }
        filterAndAddAreaRoute(
            *routes,
            areaName,
            routeFilterType,
            it->second,
            filter.prefixType());
      }
    } else {
      for (auto& [prefix, prefixEntries] : prefixMap_) {
        filterAndAddAreaRoute(
            *routes,
            areaName,
            routeFilterType,
            prefixEntries,
            filter.prefixType());
      }
    }
    promise.setValue(std::move(routes));
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
          supportingRoutes.size() >= *prefix.minimum_supporting_routes());
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
    folly::CIDRNetwork const& prefix,
    std::unordered_map<thrift::PrefixType, PrefixEntry> const& prefixEntries) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::AdvertisedRouteDetail routeDetail;
  routeDetail.prefix() = toIpPrefix(prefix);

  // Add best route selection data
  for (auto& prefixType : selectBestPrefixMetrics(prefixEntries)) {
    routeDetail.bestKeys()->emplace_back(prefixType);
  }
  routeDetail.bestKey() = routeDetail.bestKeys()->at(0);

  // Add prefix entries and honor the filter
  for (auto& [prefixType, prefixEntry] : prefixEntries) {
    if (typeFilter && *typeFilter != prefixType) {
      continue;
    }
    routeDetail.routes()->emplace_back();
    auto& route = routeDetail.routes()->back();
    route.key() = prefixType;
    route.route() = *prefixEntry.tPrefixEntry;
    route.igpCost() = prefixEntry.policyMatchData.igpCost;
  }

  // Add detail if there are entries to return
  if (routeDetail.routes()->size()) {
    routes.emplace_back(std::move(routeDetail));
  }
}

void
PrefixManager::filterAndAddAreaRoute(
    std::vector<thrift::AdvertisedRoute>& routes,
    const std::string& area,
    const thrift::RouteFilterType& routeFilterType,
    std::unordered_map<thrift::PrefixType, PrefixEntry> const& prefixEntries,
    apache::thrift::optional_field_ref<thrift::PrefixType&> const& typeFilter) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  auto bestTypeEntry = getBestPrefixEntry(prefixEntries);
  thrift::PrefixType bestPrefixType = bestTypeEntry.first;
  const auto& bestPrefixEntry = bestTypeEntry.second;

  // The prefix will not be advertised to user provided area
  if (!bestPrefixEntry.dstAreas.count(area)) {
    return;
  }
  // return if type does not match
  if (typeFilter && *typeFilter != bestPrefixType) {
    return;
  }

  const auto& prePolicyTPrefixEntry = bestPrefixEntry.tPrefixEntry;
  thrift::AdvertisedRoute route;
  route.igpCost() = bestPrefixEntry.policyMatchData.igpCost;
  route.key() = bestPrefixType;

  // prefilter advertised route
  if (routeFilterType == thrift::RouteFilterType::PREFILTER_ADVERTISED) {
    route.route() = *prePolicyTPrefixEntry;
    routes.emplace_back(std::move(route));
    return;
  }

  // run policy
  std::shared_ptr<thrift::PrefixEntry> postPolicyTPrefixEntry;
  std::string hitPolicyName{};

  const auto& policy = areaToPolicy_.at(area);
  if (policy) {
    std::tie(postPolicyTPrefixEntry, hitPolicyName) =
        policyManager_->applyPolicy(
            *policy,
            prePolicyTPrefixEntry,
            std::nullopt /* policy Action Data */,
            bestPrefixEntry.policyMatchData);
  } else {
    postPolicyTPrefixEntry = prePolicyTPrefixEntry;
  }

  if (routeFilterType == thrift::RouteFilterType::POSTFILTER_ADVERTISED &&
      postPolicyTPrefixEntry) {
    // add post filter advertised route
    route.route() = *postPolicyTPrefixEntry;
    if (!hitPolicyName.empty()) {
      route.hitPolicy() = hitPolicyName;
    }
    routes.emplace_back(std::move(route));
    return;
  }

  if (routeFilterType == thrift::RouteFilterType::REJECTED_ON_ADVERTISE &&
      !postPolicyTPrefixEntry) {
    // add post filter rejected route
    route.route() = *prePolicyTPrefixEntry;
    route.hitPolicy() = hitPolicyName;
    routes.emplace_back(std::move(route));
  }
}

std::vector<PrefixEntry>
PrefixManager::applyOriginationPolicy(
    const std::vector<PrefixEntry>& prefixEntries,
    const std::string& policyName) {
  // Store them before applying origination policy for future debugging
  // purpose
  storeOriginatedPrefixes(prefixEntries, policyName);
  std::vector<PrefixEntry> postOriginationPrefixes = {};
  for (auto prefix : prefixEntries) {
    auto [postPolicyTPrefixEntry, _] = policyManager_->applyPolicy(
        policyName,
        prefix.tPrefixEntry,
        prefix.policyActionData,
        prefix.policyMatchData);
    if (postPolicyTPrefixEntry) {
      XLOG(DBG1) << fmt::format(
          "Prefixes {} : accepted/modified by origination policy {}",
          folly::IPAddress::networkToString(prefix.network),
          policyName);
      auto dstAreasCp = prefix.dstAreas;
      postOriginationPrefixes.emplace_back(
          std::move(postPolicyTPrefixEntry),
          std::move(dstAreasCp),
          std::move(prefix.nexthops));
    } else {
      XLOG(DBG1) << fmt::format(
          "Not processing prefixes {} : denied by origination policy {}",
          folly::IPAddress::networkToString(prefix.network),
          policyName);
    }
  }
  return postOriginationPrefixes;
}

bool
PrefixManager::advertisePrefixesImpl(
    std::vector<thrift::PrefixEntry>&& tPrefixEntries,
    const std::unordered_set<std::string>& dstAreas,
    const std::optional<std::string>& policyName) {
  if (tPrefixEntries.empty()) {
    return false;
  }
  std::vector<PrefixEntry> toAddOrUpdate;
  for (auto& tPrefixEntry : tPrefixEntries) {
    auto dstAreasCp = dstAreas;
    toAddOrUpdate.emplace_back(
        std::make_shared<thrift::PrefixEntry>(std::move(tPrefixEntry)),
        std::move(dstAreasCp));
  }
  return advertisePrefixesImpl(toAddOrUpdate, policyName);
}

bool
PrefixManager::advertisePrefixesImpl(
    std::vector<PrefixEntry>&& prefixEntries,
    const std::unordered_set<std::string>& dstAreas,
    const std::optional<std::string>& policyName) {
  if (prefixEntries.empty()) {
    return false;
  }

  std::vector<PrefixEntry> toAddOrUpdate;
  for (auto& prefixEntry : prefixEntries) {
    auto dstAreasCp = dstAreas;
    prefixEntry.dstAreas = std::move(dstAreasCp);

    // Create PrefixEntry and set unicastRotues
    toAddOrUpdate.push_back(std::move(prefixEntry));
  }
  return advertisePrefixesImpl(toAddOrUpdate, policyName);
}

bool
PrefixManager::advertisePrefixesImpl(
    const std::vector<PrefixEntry>& prefixEntries,
    const std::optional<std::string>& policyName) {
  // Apply origination policy first if event came with one
  const auto& postOriginationPrefixes = policyName
      ? applyOriginationPolicy(prefixEntries, *policyName)
      : prefixEntries;

  bool updated{false};
  for (const auto& entry : postOriginationPrefixes) {
    const auto& type = *entry.tPrefixEntry->type();
    const auto& prefixCidr = entry.network;

    // ATTN: create new folly::CIDRNetwork -> typeToPrefixes
    //       mapping if it is new prefix. `[]` operator is
    //       used intentionally.
    auto [it, inserted] = prefixMap_[prefixCidr].emplace(type, entry);

    if (!inserted) {
      if (it->second == entry) {
        // Case 1: ignore SAME `PrefixEntry`
        continue;
      }
      // Case 2: update existing `PrefixEntry`
      it->second = entry;
    }
    // Case 3: store pendingUpdate for batch processing
    pendingUpdates_.addPrefixChange(prefixCidr);
    updated = true;
  }

  if (updated) {
    // schedule `syncKvStore` after throttled timeout
    syncKvStoreThrottled_->operator()();
  }

  return updated;
}

bool
PrefixManager::withdrawPrefixesImpl(
    const std::vector<thrift::PrefixEntry>& tPrefixEntries) {
  if (tPrefixEntries.empty()) {
    return false;
  }

  // Remove prefix from originated prefixes map
  removeOriginatedPrefixes(tPrefixEntries);

  bool updated{false};
  for (const auto& prefixEntry : tPrefixEntries) {
    const auto& type = *prefixEntry.type();
    const auto& prefixCidr = toIPNetwork(*prefixEntry.prefix());

    // iterator usage to avoid multiple times of map access
    auto typeIt = prefixMap_.find(prefixCidr);

    // ONLY populate changed collection when successfully erased key
    if (typeIt != prefixMap_.end() && typeIt->second.erase(type)) {
      updated = true;
      // store pendingUpdate for batch processing
      pendingUpdates_.addPrefixChange(prefixCidr);
      // clean up data structure
      if (typeIt->second.empty()) {
        prefixMap_.erase(prefixCidr);
      }
    }
  }

  if (updated) {
    // schedule `syncKvStore` after throttled timeout
    syncKvStoreThrottled_->operator()();
  }

  return updated;
}

bool
PrefixManager::withdrawPrefixEntriesImpl(
    const std::vector<PrefixEntry>& prefixEntries) {
  if (prefixEntries.empty()) {
    return false;
  }

  // Remove prefix from originated prefixes map
  removeOriginatedPrefixes(prefixEntries);

  bool updated{false};
  for (const auto& prefixEntry : prefixEntries) {
    const auto& type = *prefixEntry.tPrefixEntry->type();

    // iterator usage to avoid multiple times of map access
    auto typeIt = prefixMap_.find(prefixEntry.network);

    // ONLY populate changed collection when successfully erased key
    if (typeIt != prefixMap_.end() && typeIt->second.erase(type)) {
      updated = true;
      // store pendingUpdate for batch processing
      pendingUpdates_.addPrefixChange(prefixEntry.network);
      // clean up data structure
      if (typeIt->second.empty()) {
        prefixMap_.erase(prefixEntry.network);
      }
    }
  }

  if (updated) {
    // schedule `syncKvStore` after throttled timeout
    syncKvStoreThrottled_->operator()();
  }

  return updated;
}

bool
PrefixManager::syncPrefixesByTypeImpl(
    thrift::PrefixType type,
    const std::vector<thrift::PrefixEntry>& tPrefixEntries,
    const std::unordered_set<std::string>& dstAreas,
    const std::optional<std::string>& policyName) {
  XLOG(DBG1) << "Syncing prefixes of type " << toString(type);
  // building these lists so we can call add and remove and get detailed
  // logging
  std::vector<thrift::PrefixEntry> toAddOrUpdate, toRemove;
  std::unordered_set<folly::CIDRNetwork> toRemoveSet;
  for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
    if (typeToPrefixes.count(type)) {
      toRemoveSet.emplace(prefix);
    }
  }
  for (auto const& entry : tPrefixEntries) {
    CHECK(type == *entry.type());
    toRemoveSet.erase(toIPNetwork(*entry.prefix()));
    toAddOrUpdate.emplace_back(entry);
  }
  for (auto const& prefix : toRemoveSet) {
    toRemove.emplace_back(*prefixMap_.at(prefix).at(type).tPrefixEntry);
  }
  bool updated = false;
  updated |=
      ((advertisePrefixesImpl(std::move(toAddOrUpdate), dstAreas, policyName))
           ? 1
           : 0);
  updated |= ((withdrawPrefixesImpl(toRemove)) ? 1 : 0);
  return updated;
}

bool
PrefixManager::withdrawPrefixesByTypeImpl(thrift::PrefixType type) {
  std::vector<thrift::PrefixEntry> toRemove;
  for (auto const& [prefix, typeToPrefixes] : prefixMap_) {
    auto it = typeToPrefixes.find(type);
    if (it != typeToPrefixes.end()) {
      toRemove.emplace_back(*it->second.tPrefixEntry);
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
  if (!inserted) {
    return;
  }

  for (auto& [network, route] : originatedPrefixDb_) {
    // folly::CIDRNetwork.first -> IPAddress
    // folly::CIDRNetwork.second -> cidr length
    if (!prefix.first.inSubnet(network.first, network.second)) {
      continue;
    }

    XLOG(DBG1) << "[Route Origination] Adding supporting route "
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

    XLOG(DBG1) << "[Route Origination] Removing supporting route "
               << folly::IPAddress::networkToString(prefix)
               << " for originated route "
               << folly::IPAddress::networkToString(network);

    route.supportingRoutes.erase(prefix);
  }

  // clean local caching
  ribPrefixDb_.erase(prefix);
}

void
PrefixManager::processOriginatedPrefixes() {
  std::vector<PrefixEntry> advertisedPrefixes{};
  std::vector<thrift::PrefixEntry> withdrawnPrefixes{};
  DecisionRouteUpdate routeUpdatesForDecision;
  routeUpdatesForDecision.prefixType = thrift::PrefixType::CONFIG;

  for (auto& [network, route] : originatedPrefixDb_) {
    if (route.shouldAdvertise()) {
      route.isAdvertised = true; // mark as advertised
      advertisedPrefixes.emplace_back(
          std::make_shared<thrift::PrefixEntry>(
              route.unicastEntry.bestPrefixEntry),
          allAreaIds());
      if (route.originatedPrefix.install_to_fib().has_value() &&
          *route.originatedPrefix.install_to_fib()) {
        advertisedPrefixes.back().nexthops = route.unicastEntry.nexthops;
      }
      XLOG(INFO) << "[Route Origination] Advertising originated route "
                 << folly::IPAddress::networkToString(network);
    }

    if (route.shouldWithdraw()) {
      route.isAdvertised = false; // mark as withdrawn
      withdrawnPrefixes.emplace_back(
          createPrefixEntry(toIpPrefix(network), thrift::PrefixType::CONFIG));
      XLOG(DBG1) << "[Route Origination] Withdrawing originated route "
                 << folly::IPAddress::networkToString(network);
    } else {
      // In OpenR initialization process, PrefixManager publishes unicast
      // routes for config originated prefixes with `install_to_fib=true`
      // (This is required in warmboot). If it turns out there are no enough
      // supporting routes, previously published unicast routes in OpenR
      // initialization process should be deleted.
      // TODO: Consider moving static route generation and best entry
      // selection logic to Decision.
      if (!route.supportingRoutesFulfilled()) {
        auto it = advertiseStatus_.find(network);
        if (it != advertiseStatus_.end() &&
            it->second.publishedRoute.has_value()) {
          routeUpdatesForDecision.unicastRoutesToDelete.emplace_back(network);
          it->second.publishedRoute.reset();
        }
      }
    }
  }
  // advertise originated config routes to KvStore
  advertisePrefixesImpl(advertisedPrefixes);
  withdrawPrefixesImpl(withdrawnPrefixes);

  if (!routeUpdatesForDecision.empty()) {
    staticRouteUpdatesQueue_.push(std::move(routeUpdatesForDecision));
  }
}

void
PrefixManager::processFibRouteUpdates(DecisionRouteUpdate&& fibRouteUpdate) {
  // Store update type first
  auto type = fibRouteUpdate.type;

  // Store programmed label/unicast routes info.
  storeProgrammedRoutes(fibRouteUpdate);

  // Re-advertise prefixes received from one area to other areas.
  redistributePrefixesAcrossAreas(std::move(fibRouteUpdate));

  if (type == DecisionRouteUpdate::FULL_SYNC &&
      uninitializedPrefixTypes_.erase(thrift::PrefixType::RIB)) {
    XLOG(INFO) << "[Initialization] Received initial RIB type routes.";
    triggerInitialPrefixDbSync();
  }
}

void
PrefixManager::storeProgrammedRoutes(
    const DecisionRouteUpdate& fibRouteUpdates) {
  // In case of full sync, reset previous stored programmed routes.
  if (fibRouteUpdates.type == DecisionRouteUpdate::FULL_SYNC) {
    for (const auto& deletedPrefix : programmedPrefixes_) {
      pendingUpdates_.addPrefixChange(deletedPrefix);
    }
    programmedPrefixes_.clear();
  }

  // Record unicast routes from OpenR/Fib.
  for (const auto& [prefix, _] : fibRouteUpdates.unicastRoutesToUpdate) {
    if (programmedPrefixes_.insert(prefix).second /*inserted*/) {
      pendingUpdates_.addPrefixChange(prefix);
    }
  }
  for (const auto& prefix : fibRouteUpdates.unicastRoutesToDelete) {
    if (programmedPrefixes_.erase(prefix) /*erased*/) {
      pendingUpdates_.addPrefixChange(prefix);
    }
  }

  // schedule `syncKvStore` after throttled timeout
  syncKvStoreThrottled_->operator()();
}

void
PrefixManager::initCounters() noexcept {
  fb303::fbData->addStatExportType(
      "prefix_manager.route_advertisements", fb303::SUM);
  fb303::fbData->addStatExportType(
      "prefix_manager.route_withdraws", fb303::SUM);
  fb303::fbData->addStatExportType(
      "prefix_manager.originated_routes", fb303::SUM);
  fb303::fbData->addStatExportType("prefix_manager.rejected", fb303::SUM);
  for (auto const& area : allAreaIds()) {
    fb303::fbData->addStatExportType(
        fmt::format("prefix_manager.route_advertisements.{}", area),
        fb303::SUM);
    fb303::fbData->addStatExportType(
        fmt::format("prefix_manager.rejected.{}", area), fb303::SUM);
  }
}

namespace {
void
resetNonTransitiveAttrs(thrift::PrefixEntry& prefixEntry) {
  // Reset non-transitive attributes which cannot be redistributed across
  // areas. Ref:
  // https://openr.readthedocs.io/Operator_Guide/RouteRepresentation.html.
  prefixEntry.forwardingAlgorithm() =
      thrift::PrefixForwardingAlgorithm::SP_ECMP;
  prefixEntry.forwardingType() = thrift::PrefixForwardingType::IP;
  prefixEntry.minNexthop().reset();
  prefixEntry.weight().reset();
}
} // namespace

void
PrefixManager::redistributePrefixesAcrossAreas(
    DecisionRouteUpdate&& fibRouteUpdate) {
  XLOGF(
      DBG1,
      "Processing FibRouteUpdate: {} announcements, {} withdrawals",
      fibRouteUpdate.unicastRoutesToUpdate.size(),
      fibRouteUpdate.unicastRoutesToDelete.size());

  std::vector<PrefixEntry> advertisedPrefixes{};
  std::vector<thrift::PrefixEntry> withdrawnPrefixes{};

  // ATTN: Routes imported from local BGP won't show up inside
  // `fibRouteUpdate`. However, local-originated static route
  // (e.g. from route-aggregation) can come along.

  // Add/Update unicast routes
  for (auto& [prefix, route] : fibRouteUpdate.unicastRoutesToUpdate) {
    // NOTE: future expansion - run egress policy here

    //
    // Cross area, modify attributes
    //
    auto& prefixEntry = route.bestPrefixEntry;

    if (*prefixEntry.type() == thrift::PrefixType::CONFIG) {
      // Skip local-originated prefix as it won't be considered as
      // part of its own supporting routes.
      if (originatedPrefixDb_.count(prefix)) {
        continue;
      }
    }

    // Update interested mutable transitive attributes.
    //
    // For OpenR route representation, referring to
    // https://openr.readthedocs.io/Operator_Guide/RouteRepresentation.html
    // 1. append area stack
    prefixEntry.area_stack()->emplace_back(route.bestArea);
    // 2. increase distance by 1
    ++(*prefixEntry.metrics()->distance());
    // 3. normalize to RIB routes
    prefixEntry.type() = thrift::PrefixType::RIB;

    // Keep weight in OpenrPolicyActionData. Area policy will later decide
    // whether it should be advertised into one area.
    std::optional<OpenrPolicyActionData> policyActionData{std::nullopt};
    if (prefixEntry.weight().has_value()) {
      policyActionData = OpenrPolicyActionData(prefixEntry.weight().value());
    }

    OpenrPolicyMatchData policyMatchData(route.igpCost);

    // Reset non-transitive attributes before redistribution across areas.
    resetNonTransitiveAttrs(prefixEntry);

    // Populate routes to be advertised to KvStore
    auto dstAreas = allAreaIds();
    for (const auto& nh : route.nexthops) {
      if (nh.area().has_value()) {
        dstAreas.erase(*nh.area());
      }
    }
    advertisedPrefixes.emplace_back(
        std::make_shared<thrift::PrefixEntry>(std::move(prefixEntry)),
        std::move(dstAreas),
        policyActionData,
        policyMatchData,
        route.localRouteConsidered /* prefer over local*/);

    // Adjust supporting route count due to prefix advertisement
    aggregatesToAdvertise(prefix);
  }

  // Delete unicast routes
  for (const auto& prefix : fibRouteUpdate.unicastRoutesToDelete) {
    // TODO: remove this when advertise RibUnicastEntry for routes to delete
    if (originatedPrefixDb_.count(prefix)) {
      // skip local-originated prefix as it won't be considered as
      // part of its own supporting routes.
      continue;
    }

    // Routes to be withdrawn via KvStore
    withdrawnPrefixes.emplace_back(
        createPrefixEntry(toIpPrefix(prefix), thrift::PrefixType::RIB));

    // adjust supporting route count due to prefix withdrawn
    aggregatesToWithdraw(prefix);
  }

  // Maybe advertise/withdrawn for local originated routes
  processOriginatedPrefixes();

  // Redisrtibute RIB route ONLY when there are multiple `areaId` configured .
  // We want to keep processFibRouteUpdates() running as dynamic
  // configuration could add/remove areas.
  if (areaToPolicy_.size() > 1) {
    advertisePrefixesImpl(advertisedPrefixes);
    withdrawPrefixesImpl(withdrawnPrefixes);
  }

  // ignore mpls updates
}

std::unordered_set<std::string>
PrefixManager::allAreaIds() {
  std::unordered_set<std::string> allAreaIds;
  for (const auto& [area, _] : areaToPolicy_) {
    allAreaIds.emplace(area);
  }
  return allAreaIds;
}

} // namespace openr
