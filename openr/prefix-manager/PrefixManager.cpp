/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManager.h"
#include "folly/IPAddress.h"

#include <fb303/ServiceData.h>
#include <folly/futures/Future.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <optional>
#include <utility>
#ifndef NO_FOLLY_EXCEPTION_TRACER
#include <folly/experimental/exception_tracer/ExceptionTracer.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

namespace fb303 = facebook::fb303;

PrefixManager::PrefixManager(
    messaging::ReplicateQueue<DecisionRouteUpdate>& staticRouteUpdatesQueue,
    messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue,
    messaging::RQueue<Publication> kvStoreUpdatesQueue,
    messaging::RQueue<PrefixEvent> prefixUpdatesQueue,
    messaging::RQueue<DecisionRouteUpdate> fibRouteUpdatesQueue,
    std::shared_ptr<const Config> config,
    KvStore* kvStore)
    : nodeId_(config->getNodeName()),
      config_(config),
      ttlKeyInKvStore_(std::chrono::milliseconds(
          *config->getKvStoreConfig().key_ttl_ms_ref())),
      staticRouteUpdatesQueue_(staticRouteUpdatesQueue),
      kvRequestQueue_(kvRequestQueue),
      v4OverV6Nexthop_(config->isV4OverV6NexthopEnabled()),
      kvStore_(kvStore),
      preferOpenrOriginatedRoutes_(
          config->getConfig().get_prefer_openr_originated_routes()),
      enableNewPrefixFormat_(
          config->getConfig().get_enable_new_prefix_format()) {
  CHECK(kvStore_);
  CHECK(config);

  // Always add RIB type prefixes, since Fib routes updates are always expected
  // in OpenR initialization procedure.
  uninitializedPrefixTypes_.emplace(thrift::PrefixType::RIB);
  if (config->getConfig().get_enable_bgp_peering()) {
    uninitializedPrefixTypes_.emplace(thrift::PrefixType::BGP);
  }
  // TODO: Handle VIP after VipInjector supports GR.

  if (auto policyConf = config->getAreaPolicies()) {
    policyManager_ = std::make_unique<PolicyManager>(*policyConf);
  }

  for (const auto& [areaId, areaConf] : config->getAreas()) {
    areaToPolicy_.emplace(areaId, areaConf.getImportPolicyName());
  }

  //
  // Hold time for synchronizing prefixes in KvStore. We expect all the
  // prefixes to be recovered (Redistribute, Plugin etc.) within this time
  // window.
  // NOTE: Based on signals from sources that advertises the routes we can
  // synchronize prefixes earlier. This time provides worst case bound.
  //
  const std::chrono::seconds initialPrefixHoldTime{
      *config->getConfig().prefix_hold_time_s_ref()};

  // Create KvStore client
  kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
      this,
      nodeId_,
      kvStore_,
      false /* createKvStoreUpdatesReader */,
      true /* useThrottle */);

  // Create initial timer to update all prefixes after HoldTime (2 * KA)
  // TODO: deprecate this after prefix_hold_time_s after OpenR initialization
  // procedure is stable.
  if (not config_->getConfig().get_enable_initialization_process()) {
    initialSyncKvStoreTimer_ =
        folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
          LOG(INFO) << "syncKvStore() from initialSyncKvStoreTimer_.";
          syncKvStore();
        });
    initialSyncKvStoreTimer_->scheduleTimeout(initialPrefixHoldTime);
  }

  // Create throttled update state
  syncKvStoreThrottled_ = std::make_unique<AsyncThrottle>(
      getEvb(), Constants::kKvStoreSyncThrottleTimeout, [this]() noexcept {
        // No write to KvStore before initial KvStore sync
        if (config_->getConfig().get_enable_initialization_process()) {
          // Signal based initialization process.
          if ((not uninitializedPrefixTypes_.empty()) or
              (not initialKvStoreSynced_)) {
            return;
          }
        } else {
          // Timer based initialization process.
          if (initialSyncKvStoreTimer_->isScheduled()) {
            return;
          }
        }

        syncKvStore();
      });

  // Load openrConfig for local-originated routes
  if (auto prefixes = config->getConfig().originated_prefixes_ref()) {
    // read originated prefixes from OpenrConfig
    buildOriginatedPrefixDb(*prefixes);

    // ATTN: consider min_supporting_route = 0, immediately advertise
    // originated routes to `KvStore`
    processOriginatedPrefixes();
    LOG(INFO) << "[Initialization] Originated prefixes processed.";
  }

  // Schedule fiber to read prefix updates messages
  addFiberTask([q = std::move(prefixUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeUpdate = q.get(); // perform read
      if (maybeUpdate.hasError()) {
        VLOG(1) << "Terminating prefix update request processing fiber";
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
        advertisePrefixesImpl(std::move(update.prefixes), dstAreas);
        advertisePrefixesImpl(
            std::move(std::move(update.prefixEntries)),
            dstAreas,
            update.policyName);

        if (uninitializedPrefixTypes_.erase(update.type)) {
          // Received initial prefixes of certain type in OpenR initialization
          // process.
          LOG(INFO) << fmt::format(
              "[Initialization] Received {} prefixes of type {}.",
              update.prefixes.size() + update.prefixEntries.size(),
              apache::thrift::util::enumNameSafe<thrift::PrefixType>(
                  update.type));
          triggerInitialPrefixDbSync();
        }
        break;
      }
      case PrefixEventType::WITHDRAW_PREFIXES:
        withdrawPrefixesImpl(update.prefixes);
        withdrawPrefixEntriesImpl(update.prefixEntries);
        break;
      case PrefixEventType::WITHDRAW_PREFIXES_BY_TYPE:
        withdrawPrefixesByTypeImpl(update.type);
        break;
      case PrefixEventType::SYNC_PREFIXES_BY_TYPE:
        syncPrefixesByTypeImpl(
            update.type, update.prefixes, dstAreas, update.policyName);
        break;
      default:
        LOG(ERROR) << "Unknown command received. "
                   << static_cast<int>(update.eventType);
      }
    }
  });

  // Fiber to process route updates from Fib.
  addFiberTask([q = std::move(fibRouteUpdatesQueue), this]() mutable noexcept {
    while (true) {
      auto maybeThriftObj = q.get(); // perform read
      if (maybeThriftObj.hasError()) {
        VLOG(1) << "Terminating route delta processing fiber";
        break;
      }

      try {
        VLOG(2) << "Received RIB updates from Decision";
        processFibRouteUpdates(std::move(maybeThriftObj).value());
      } catch (const std::exception&) {
#ifndef NO_FOLLY_EXCEPTION_TRACER
        // collect stack strace then fail the process
        for (auto& exInfo : folly::exception_tracer::getCurrentExceptions()) {
          LOG(ERROR) << exInfo;
        }
#endif
        throw;
      }
    }
  });

  // Fiber to process publication from KvStore
  addFiberTask([q = std::move(kvStoreUpdatesQueue), this]() mutable noexcept {
    LOG(INFO) << "Starting KvStore updates processing fiber";
    while (true) {
      auto maybePub = q.get(); // perform read
      if (maybePub.hasError()) {
        VLOG(1) << fmt::format(
            "Terminating KvStore updates processing fiber, error: {}",
            maybePub.error());
        break;
      }
      if (not maybePub.hasValue()) {
        continue;
      }
      auto& pub = maybePub.value();
      // kvStoreSynced and tPublication are exclusive with each other.
      if (pub.kvStoreSynced) {
        // "kvStoreSynced" signal is used to mark initial KvStore sync stage
        // done during Open/R initialization process. PrefixManager could dump
        // all prefixes advertised by itself previously.
        dumpSelfOriginatedPrefixes();
        LOG(INFO)
            << "[Initialization] All prefix keys are retrieved from KvStore.";
        initialKvStoreSynced_ = true;
        triggerInitialPrefixDbSync();
      } else {
        // TODO: Do not call KvStoreClient_ to process publications after
        // persistKey() and clearKey() are natively supported in KvStore.
        kvStoreClient_->processPublication(pub.tPublication);

        // Process KvStore Thrift publication.
        processPublication(std::move(pub.tPublication));
      }
    }
  });
}

void
PrefixManager::dumpSelfOriginatedPrefixes() {
  // get initial dump of keys related to `myNodeId_`.
  // ATTN: when Open/R restarts, newly started prefixManager will need to
  // understand what it has previously advertised.
  const auto keyPrefix =
      fmt::format("{}{}:", Constants::kPrefixDbMarker.toString(), nodeId_);

  for (const auto& [area, _] : areaToPolicy_) {
    auto result = kvStoreClient_->dumpAllWithPrefix(AreaId{area}, keyPrefix);
    if (not result.has_value()) {
      LOG(ERROR) << "Failed dumping prefix " << keyPrefix << " from area "
                 << area;
      continue;
    }

    folly::small_vector<folly::CIDRNetwork> changed;
    for (auto const& [keyStr, _] : result.value()) {
      // TODO: avoid decoding keys
      auto maybePrefixKey = PrefixKey::fromStr(keyStr);
      if (maybePrefixKey.hasError()) {
        // this is bad format of key.
        LOG(ERROR) << fmt::format(
            "Unable to parse prefix key: {} with error: {}",
            keyStr,
            maybePrefixKey.error());
        continue;
      }

      // ATTN: to avoid prefix churn, skip processing prefixes from previous
      // incarnation with different prefix key format.
      if (enableNewPrefixFormat_ != maybePrefixKey.value().isPrefixKeyV2()) {
        const std::string version =
            maybePrefixKey.value().isPrefixKeyV2() ? "v2" : "v1";
        LOG(INFO) << fmt::format(
            "Skip processing {} format of prefix: {}", version, keyStr);
        continue;
      }

      // populate keysInKvStore_ collection to make sure we can find
      // key when clear key from `KvStore`
      const auto network = maybePrefixKey->getCIDRNetwork();
      keysInKvStore_[network].keys.emplace(keyStr);

      // Populate pendingState to check keys
      pendingUpdates_.addPrefixChange(network);
    }
    // populate pending update in one shot
    syncKvStoreThrottled_->operator()();
  }
}

void
PrefixManager::processPublication(thrift::Publication&& thriftPub) {
  folly::small_vector<folly::CIDRNetwork> changed{};
  for (const auto& [keyStr, val] : *thriftPub.keyVals_ref()) {
    // Only interested in prefix updates.
    // Ignore if val has no value field inside `thrift::Value`(e.g. ttl update)
    if (not(keyStr.find(Constants::kPrefixDbMarker.toString()) == 0) or
        (not val.value_ref())) {
      continue;
    }
    auto prefixKey = PrefixKey::fromStr(keyStr);
    // Skip bad format of key.
    if (prefixKey.hasError()) {
      LOG(ERROR) << fmt::format(
          "Unable to parse prefix key: {} with error: {}",
          keyStr,
          prefixKey.error());
      continue;
    }
    // Skip none-self advertised prefixes or already persisted keys.
    if (prefixKey->getNodeName() != nodeId_ or
        keysInKvStore_.count(prefixKey->getCIDRNetwork()) > 0) {
      continue;
    }
    // ATTN: to avoid prefix churn, skip processing prefixes from previous
    // incarnation with different prefix key format.
    if (enableNewPrefixFormat_ != prefixKey.value().isPrefixKeyV2()) {
      const std::string version =
          prefixKey.value().isPrefixKeyV2() ? "v2" : "v1";
      LOG(INFO) << fmt::format(
          "Skip processing {} format of prefix: {}", version, keyStr);
      return;
    }
    try {
      const auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
          *val.value_ref(), serializer_);
      if (not *prefixDb.deletePrefix_ref()) {
        VLOG(2) << "Learning previously announced prefix: " << keyStr;
        // populate keysInKvStore_ collection to make sure we can find
        // key when clear key from `KvStore`
        const auto network = prefixKey->getCIDRNetwork();
        keysInKvStore_[network].keys.emplace(keyStr);

        // Populate pendingState to check keys
        pendingUpdates_.addPrefixChange(network);
        syncKvStoreThrottled_->operator()();
      }
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to deserialize corresponding value for key "
                 << keyStr << ". Exception: " << folly::exceptionStr(ex);
    }
  } // for
}

PrefixManager::~PrefixManager() {
  // - If EventBase is stopped or it is within the evb thread, run immediately;
  // - Otherwise, will wait the EventBase to run;
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    initialSyncKvStoreTimer_.reset();
    syncKvStoreThrottled_.reset();
  });
  kvStoreClient_.reset();
}

void
PrefixManager::stop() {
  // Stop KvStoreClient first
  kvStoreClient_->stop();
  VLOG(1) << "KvStoreClient successfully stopped.";

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

thrift::PrefixEntry
PrefixManager::toPrefixEntryThrift(
    const thrift::OriginatedPrefix& prefix, const thrift::PrefixType& tType) {
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
  entry.prefix_ref() =
      toIpPrefix(folly::IPAddress::createNetwork(*prefix.prefix_ref()));
  entry.metrics_ref() = std::move(metrics);
  // ATTN: local-originated prefix has unique type CONFIG
  //      to be differentiated from others.
  entry.type_ref() = tType;
  // ATTN: `area_stack` will be explicitly set to empty
  //      as there is no "cross-area" behavior for local
  //      originated prefixes.
  CHECK(entry.area_stack_ref()->empty());
  if (auto tags = prefix.tags_ref()) {
    entry.tags_ref() = *tags;
  }
  return entry;
}

void
PrefixManager::buildOriginatedPrefixDb(
    const std::vector<thrift::OriginatedPrefix>& prefixes) {
  for (const auto& prefix : prefixes) {
    auto network = folly::IPAddress::createNetwork(*prefix.prefix_ref());
    auto nh = network.first.isV4() and not v4OverV6Nexthop_
        ? Constants::kLocalRouteNexthopV4.toString()
        : Constants::kLocalRouteNexthopV6.toString();

    auto entry = toPrefixEntryThrift(prefix, thrift::PrefixType::CONFIG);

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

void
PrefixManager::updatePrefixKeysInKvStore(
    const folly::CIDRNetwork& prefix,
    const PrefixEntry& prefixEntry,
    DecisionRouteUpdate& routeUpdatesOut) {
  // advertise best-entry for this prefix to `KvStore`
  auto newKeys = addKvStoreKeyHelper(prefixEntry);

  auto keysIt = keysInKvStore_.find(prefix);
  if (keysIt != keysInKvStore_.end()) {
    // ATTN: This collection holds "advertised" prefixes in previous round
    // of syncing. By removing prefixes in current run, whatever left in
    // `keysInKvStore_` will be the delta to be removed.
    for (const auto& key : newKeys) {
      keysIt->second.keys.erase(key);
    }

    // remove keys which are no longer advertised
    // e.g.
    // t0: prefix_1 => {area_1, area_2}
    // t1: prefix_1 => {area_1, area_3}
    //     (prefix_1, area_2) will be removed
    deleteKvStoreKeyHelper(keysIt->second.keys);
  }

  auto& advertisedStatus = keysInKvStore_[prefix];
  // override `keysInKvStore_` for next-round syncing
  advertisedStatus.keys = std::move(newKeys);
  // propogate route update to `KvStore` and `Decision`(if necessary)
  if (prefixEntry.shouldInstall()) {
    if (advertisedStatus.installedToFib) {
      return;
    }
    advertisedStatus.installedToFib = true;
    // Populate RibUnicastEntry struct
    // ATTN: AREA field is empty for NHs
    // if shouldInstall() is true, nexthops is guaranteed to have value.
    RibUnicastEntry unicastEntry(prefix, prefixEntry.nexthops.value());
    unicastEntry.bestPrefixEntry = *prefixEntry.tPrefixEntry;
    routeUpdatesOut.addRouteToUpdate(std::move(unicastEntry));
  } else {
    // if was installed to fib, but now lose in tie break, withdraw from
    // fib.
    if (advertisedStatus.installedToFib) {
      routeUpdatesOut.unicastRoutesToDelete.emplace_back(prefix);
      advertisedStatus.installedToFib = false;
    }
  } // else
}

std::unordered_set<std::string>
PrefixManager::addKvStoreKeyHelper(const PrefixEntry& entry) {
  std::unordered_set<std::string> prefixKeys;
  const auto& tPrefixEntry = entry.tPrefixEntry;
  const auto& type = *tPrefixEntry->type_ref();
  const std::unordered_set<std::string> areaStack{
      tPrefixEntry->area_stack_ref()->begin(),
      tPrefixEntry->area_stack_ref()->end()};

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
          policyManager_->applyPolicy(*policy, tPrefixEntry);

      // policy reject prefix, nothing to do.
      if (not postPolicyTPrefixEntry) {
        VLOG(2) << "[Area Policy] " << *policy << " rejected prefix: "
                << "(Type, PrefixEntry): (" << toString(type) << ", "
                << toString(*tPrefixEntry, true) << "), hit term ("
                << hitPolicyName << ")";
        continue;
      }

      // policy accept prefix, go ahread with prefix announcement.
      VLOG(2) << "[Area Policy] " << *policy << " accepted/modified prefix: "
              << "(Type, PrefixEntry): (" << toString(type) << ", "
              << toString(*tPrefixEntry, true) << "), PostPolicyEntry: ("
              << toString(*postPolicyTPrefixEntry) << "), hit term ("
              << hitPolicyName << ")";
    } else {
      postPolicyTPrefixEntry = tPrefixEntry;
    }

    const auto prefixKey = PrefixKey(nodeId_, entry.network, toArea);
    const auto prefixKeyStr = enableNewPrefixFormat_
        ? prefixKey.getPrefixKeyV2()
        : prefixKey.getPrefixKey();
    auto prefixDb = createPrefixDb(nodeId_, {*postPolicyTPrefixEntry}, toArea);
    auto prefixDbStr = writeThriftObjStr(std::move(prefixDb), serializer_);

    // advertise key to `KvStore`
    if (config_->getConfig().get_enable_kvstore_request_queue()) {
      auto persistPrefixKeyVal =
          PersistKeyValueRequest(AreaId{toArea}, prefixKeyStr, prefixDbStr);
      kvRequestQueue_.push(std::move(persistPrefixKeyVal));
    } else {
      kvStoreClient_->persistKey(
          AreaId{toArea}, prefixKeyStr, prefixDbStr, ttlKeyInKvStore_);
    }

    fb303::fbData->addStatValue(
        "prefix_manager.route_advertisements", 1, fb303::SUM);
    VLOG(1) << "[Prefix Advertisement] "
            << "Area: " << toArea << ", "
            << "Type: " << toString(type) << ", "
            << toString(*postPolicyTPrefixEntry, VLOG_IS_ON(2));
    prefixKeys.emplace(prefixKeyStr);
  }
  return prefixKeys;
}

void
PrefixManager::deletePrefixKeysInKvStore(
    const folly::CIDRNetwork& prefix, DecisionRouteUpdate& routeUpdatesOut) {
  // delete actual keys being advertised in the cache
  //
  // Sample format:
  //  prefix    :    node1    :    0    :    0.0.0.0/32
  //    |              |           |             |
  //  marker        nodeId      areaId        prefixStr
  auto keysIt = keysInKvStore_.find(prefix);
  if (keysIt != keysInKvStore_.end()) {
    deleteKvStoreKeyHelper(keysIt->second.keys);
    if (keysIt->second.installedToFib) {
      routeUpdatesOut.unicastRoutesToDelete.emplace_back(prefix);
    }
    keysInKvStore_.erase(keysIt);
  }
}

void
PrefixManager::deleteKvStoreKeyHelper(
    const std::unordered_set<std::string>& deletedKeys) {
  // Prepare thrift::PrefixDatabase object for deletion
  thrift::PrefixDatabase deletedPrefixDb;
  deletedPrefixDb.thisNodeName_ref() = nodeId_;
  deletedPrefixDb.deletePrefix_ref() = true;

  // TODO: see if we can avoid encoding/decoding of string
  for (const auto& prefixStr : deletedKeys) {
    // TODO: add multi-area support for prefixStr instead of use default area
    auto maybePrefixKey = PrefixKey::fromStr(prefixStr);
    if (maybePrefixKey.hasError()) {
      // this is bad format of key.
      LOG(ERROR) << fmt::format(
          "Unable to parse prefix key: {} with error: {}",
          prefixStr,
          maybePrefixKey.error());
      continue;
    }
    const auto network = maybePrefixKey->getCIDRNetwork();
    const auto area = maybePrefixKey->getPrefixArea();

    thrift::PrefixEntry entry;
    entry.prefix_ref() = toIpPrefix(network);
    deletedPrefixDb.prefixEntries_ref() = {entry};

    // Remove prefix from KvStore and flood deletion by setting deleted value.
    if (config_->getConfig().get_enable_kvstore_request_queue()) {
      auto unsetPrefixRequest = ClearKeyValueRequest(
          AreaId{area},
          prefixStr,
          writeThriftObjStr(std::move(deletedPrefixDb), serializer_),
          true);
      kvRequestQueue_.push(std::move(unsetPrefixRequest));
    } else {
      kvStoreClient_->clearKey(
          AreaId{area},
          prefixStr,
          writeThriftObjStr(std::move(deletedPrefixDb), serializer_),
          ttlKeyInKvStore_);
    }

    VLOG(1) << "[Prefix Withdraw] "
            << "Area: " << area << ", " << toString(*entry.prefix_ref());
    fb303::fbData->addStatValue(
        "prefix_manager.route_withdraws", 1, fb303::SUM);
  }
}

void
PrefixManager::triggerInitialPrefixDbSync() {
  if (not config_->getConfig().get_enable_initialization_process()) {
    return;
  }
  if (uninitializedPrefixTypes_.empty() and initialKvStoreSynced_) {
    // Trigger initial syncKvStore(), after receiving prefixes of all expected
    // types and all inital prefix keys from KvStore.
    LOG(INFO) << "[Initialization] Triggering initial syncKvStore().";
    syncKvStore();
  }
}

bool
PrefixManager::prefixEntryReadyToBeAdvertised(const PrefixEntry& prefixEntry) {
  // Skip the check if FIB-ACK feature is disabled.
  if (not config_->getConfig().get_enable_fib_ack()) {
    return true;
  }
  // If prepend label is set, the associated label route should have been
  // programmed.
  auto labelRef = prefixEntry.tPrefixEntry->prependLabel_ref();
  if (labelRef.has_value()) {
    if (programmedLabels_.count(labelRef.value()) == 0) {
      return false;
    }
  }
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

std::vector<PrefixEntry>
PrefixManager::applyOriginationPolicy(
    const std::vector<PrefixEntry>& prefixEntries,
    const std::string& policyName) {
  // Store them before applying origination policy for future debugging purpose
  storeOriginatedPrefixes(prefixEntries, policyName);
  std::vector<PrefixEntry> postOriginationPrefixes = {};
  for (auto prefix : prefixEntries) {
    auto [postPolicyTPrefixEntry, _] =
        policyManager_->applyPolicy(policyName, prefix.tPrefixEntry);
    if (postPolicyTPrefixEntry) {
      VLOG(1) << fmt::format(
          "Prefixes {} : accepted/modified by origination policy {}",
          folly::IPAddress::networkToString(prefix.network),
          policyName);
      auto dstAreasCp = prefix.dstAreas;
      postOriginationPrefixes.emplace_back(
          std::move(postPolicyTPrefixEntry), std::move(dstAreasCp));
    } else {
      VLOG(1) << fmt::format(
          "Not processing prefixes {} : denied by origination policy {}",
          folly::IPAddress::networkToString(prefix.network),
          policyName);
    }
  }
  return postOriginationPrefixes;
}

namespace {

std::pair<thrift::PrefixType, const PrefixEntry>
getBestPrefixEntry(
    const std::unordered_map<thrift::PrefixType, PrefixEntry>&
        prefixTypeToEntry,
    bool preferOpenrOriginatedRoutes) {
  // select the best entry/entries by comparing metric_ref() field
  const auto bestTypes = selectBestPrefixMetrics(prefixTypeToEntry);
  auto bestType = *bestTypes.begin();
  // if best route is BGP, and an equivalent CONFIG route exists,
  // then prefer config route if knob prefer_openr_originated_config_=true
  if (bestType == thrift::PrefixType::BGP and preferOpenrOriginatedRoutes and
      bestTypes.count(thrift::PrefixType::CONFIG)) {
    bestType = thrift::PrefixType::CONFIG;
  }
  return std::make_pair(bestType, prefixTypeToEntry.at(bestType));
}

} // namespace

void
PrefixManager::syncKvStore() {
  VLOG(1) << "[KvStore Sync] Syncing " << pendingUpdates_.size()
          << " pending updates.";
  DecisionRouteUpdate routeUpdatesOut;
  size_t receivedPrefixCnt = 0;
  size_t syncedPrefixCnt = 0;
  size_t awaitingPrefixCnt = 0;

  // Withdraw prefixes that no longer exist.
  for (auto const& prefix : pendingUpdates_.getChangedPrefixes()) {
    auto it = prefixMap_.find(prefix);
    if (it == prefixMap_.end()) {
      // Delete prefixes that do not exist in prefixMap_.
      VLOG(1) << "Deleting keys for "
              << folly::IPAddress::networkToString(prefix);
      deletePrefixKeysInKvStore(prefix, routeUpdatesOut);
      advertisedPrefixes_.erase(prefix);
      ++syncedPrefixCnt;
    }
  }

  for (const auto& [prefix, prefixEntries] : prefixMap_) {
    receivedPrefixCnt += prefixEntries.size();

    // Check if prefix is updated and ready to be advertised.
    auto [_, bestEntry] =
        getBestPrefixEntry(prefixEntries, preferOpenrOriginatedRoutes_);
    const auto& labelRef = bestEntry.tPrefixEntry->prependLabel_ref();
    bool hasPrefixUpdate = pendingUpdates_.hasPrefix(prefix);
    bool haslabelUpdate =
        labelRef.has_value() ? pendingUpdates_.hasLabel(*labelRef) : false;
    bool readyToBeAdvertised = prefixEntryReadyToBeAdvertised(bestEntry);
    bool needToAdvertise =
        ((hasPrefixUpdate or haslabelUpdate) and readyToBeAdvertised);
    if (needToAdvertise) {
      VLOG(1) << fmt::format(
          "Adding/updating keys for {}",
          folly::IPAddress::networkToString(prefix));
      updatePrefixKeysInKvStore(prefix, bestEntry, routeUpdatesOut);
      advertisedPrefixes_[prefix] = bestEntry;
      ++syncedPrefixCnt;
      continue;
    } else if (readyToBeAdvertised) {
      // Skip still-ready-to-be and previously advertised prefix.
      CHECK(advertisedPrefixes_.count(prefix));
      continue;
    }

    // The prefix is awaiting to be advertised.
    ++awaitingPrefixCnt;

    // Check if previously advertised prefix is no longer ready to be
    // advertised.
    if (advertisedPrefixes_.count(prefix) and
        (not prefixEntryReadyToBeAdvertised(advertisedPrefixes_.at(prefix)))) {
      VLOG(1) << fmt::format(
          "Deleting advertised keys for {}",
          folly::IPAddress::networkToString(prefix));
      deletePrefixKeysInKvStore(prefix, routeUpdatesOut);
      advertisedPrefixes_.erase(prefix);
      ++syncedPrefixCnt;
    }
  } // for

  // Reset pendingUpdates_ since all pending updates are processed.
  pendingUpdates_.clear();

  // Push originatedRoutes update to staticRouteUpdatesQueue_.
  if (not routeUpdatesOut.empty()) {
    CHECK(routeUpdatesOut.mplsRoutesToUpdate.empty());
    CHECK(routeUpdatesOut.mplsRoutesToDelete.empty());
    staticRouteUpdatesQueue_.push(std::move(routeUpdatesOut));
  }

  VLOG(1) << fmt::format(
      "[KvStore Sync] Updated {} prefixes in KvStore; {} more awaiting FIB-ACK.",
      syncedPrefixCnt,
      awaitingPrefixCnt);

  // Update flat counters
  fb303::fbData->setCounter(
      "prefix_manager.received_prefixes", receivedPrefixCnt);
  // TODO: report per-area advertised prefixes if openr is running in
  // multi-areas.
  fb303::fbData->setCounter(
      "prefix_manager.advertised_prefixes", advertisedPrefixes_.size());
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
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
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
            auto it = prefixMap_.find(toIPNetwork(prefix));
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

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>
PrefixManager::getAdvertisedRoutesWithOriginationPolicy(
    thrift::RouteFilterType routeFilterType,
    thrift::AdvertisedRouteFilter filter) {
  auto [p, sf] = folly::makePromiseContract<
      std::unique_ptr<std::vector<thrift::AdvertisedRoute>>>();
  runInEventBaseThread([this,
                        p = std::move(p),
                        filter = std::move(filter),
                        routeFilterType = routeFilterType]() mutable noexcept {
    auto routes = std::make_unique<std::vector<thrift::AdvertisedRoute>>();
    if (filter.prefixes_ref()) {
      // Explicitly lookup the requested prefixes
      for (auto& prefix : filter.prefixes_ref().value()) {
        auto it = originatedPrefixMap_.find(toIPNetwork(prefix));
        if (it == originatedPrefixMap_.end()) {
          continue;
        }
        filterAndAddOriginatedRoute(
            *routes, routeFilterType, it->second, filter.prefixType_ref());
      }
    } else {
      for (auto& [prefix, prefixEntries] : originatedPrefixMap_) {
        filterAndAddOriginatedRoute(
            *routes, routeFilterType, prefixEntries, filter.prefixType_ref());
      }
    }
    p.setValue(std::move(routes));
  });
  return std::move(sf);
}

void
PrefixManager::removeOriginatedPrefixes(
    const std::vector<thrift::PrefixEntry>& prefixEntries) {
  // Remove entry from originatedPrefixMap, regardless of policy
  for (const auto& entry : prefixEntries) {
    const auto& prefixCidr = toIPNetwork(*entry.prefix_ref());
    auto typeIt = originatedPrefixMap_.find(prefixCidr);
    if (typeIt == originatedPrefixMap_.end()) {
      continue;
    }

    const auto& type = *entry.type_ref();
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
    const auto& type = *entry.tPrefixEntry->type_ref();
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
    const auto& type = *entry.tPrefixEntry->type_ref();
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
      route.key_ref() = prePolicyTPrefixEntry->get_type();
      route.route_ref() = *prePolicyTPrefixEntry;
      routes.emplace_back(std::move(route));
      continue;
    }

    auto [postPolicyTPrefixEntry, hitPolicyName] =
        policyManager_->applyPolicy(policy, prePolicyTPrefixEntry);

    if (routeFilterType == thrift::RouteFilterType::POSTFILTER_ADVERTISED and
        postPolicyTPrefixEntry) {
      // add post filter advertised route
      thrift::AdvertisedRoute route;
      route.key_ref() = prePolicyTPrefixEntry->get_type();
      route.route_ref() = *postPolicyTPrefixEntry;
      if (not hitPolicyName.empty()) {
        route.hitPolicy_ref() = hitPolicyName;
      }
      routes.emplace_back(std::move(route));
      continue;
    }

    if (routeFilterType == thrift::RouteFilterType::REJECTED_ON_ADVERTISE and
        not postPolicyTPrefixEntry) {
      // add post filter rejected route
      thrift::AdvertisedRoute route;
      route.key_ref() = prePolicyTPrefixEntry->get_type();
      route.route_ref() = *prePolicyTPrefixEntry;
      route.hitPolicy_ref() = hitPolicyName;
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
                        p = std::move(p),
                        filter = std::move(filter),
                        areaName = std::move(areaName),
                        routeFilterType = routeFilterType]() mutable noexcept {
    auto routes = std::make_unique<std::vector<thrift::AdvertisedRoute>>();
    if (filter.prefixes_ref()) {
      // Explicitly lookup the requested prefixes
      for (auto& prefix : filter.prefixes_ref().value()) {
        auto it = prefixMap_.find(toIPNetwork(prefix));
        if (it == prefixMap_.end()) {
          continue;
        }
        filterAndAddAreaRoute(
            *routes,
            areaName,
            routeFilterType,
            it->second,
            filter.prefixType_ref());
      }
    } else {
      for (auto& [prefix, prefixEntries] : prefixMap_) {
        filterAndAddAreaRoute(
            *routes,
            areaName,
            routeFilterType,
            prefixEntries,
            filter.prefixType_ref());
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
    folly::CIDRNetwork const& prefix,
    std::unordered_map<thrift::PrefixType, PrefixEntry> const& prefixEntries) {
  // Return immediately if no prefix-entry
  if (prefixEntries.empty()) {
    return;
  }

  thrift::AdvertisedRouteDetail routeDetail;
  routeDetail.prefix_ref() = toIpPrefix(prefix);

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
    route.route_ref() = *prefixEntry.tPrefixEntry;
  }

  // Add detail if there are entries to return
  if (routeDetail.routes_ref()->size()) {
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

  auto bestTypeEntry =
      getBestPrefixEntry(prefixEntries, preferOpenrOriginatedRoutes_);
  thrift::PrefixType bestPrefixType = bestTypeEntry.first;
  const auto& bestPrefixEntry = bestTypeEntry.second;

  // The prefix will not be advertised to user provided area
  if (not bestPrefixEntry.dstAreas.count(area)) {
    return;
  }
  // return if type does not match
  if (typeFilter && *typeFilter != bestPrefixType) {
    return;
  }

  const auto& prePolicyTPrefixEntry = bestPrefixEntry.tPrefixEntry;

  // prefilter advertised route
  if (routeFilterType == thrift::RouteFilterType::PREFILTER_ADVERTISED) {
    thrift::AdvertisedRoute route;
    route.set_key(bestPrefixType);
    route.set_route(*prePolicyTPrefixEntry);
    routes.emplace_back(std::move(route));
    return;
  }

  // run policy
  std::shared_ptr<thrift::PrefixEntry> postPolicyTPrefixEntry;
  std::string hitPolicyName{};

  const auto& policy = areaToPolicy_.at(area);
  if (policy) {
    std::tie(postPolicyTPrefixEntry, hitPolicyName) =
        policyManager_->applyPolicy(*policy, prePolicyTPrefixEntry);
  } else {
    postPolicyTPrefixEntry = prePolicyTPrefixEntry;
  }

  if (routeFilterType == thrift::RouteFilterType::POSTFILTER_ADVERTISED and
      postPolicyTPrefixEntry) {
    // add post filter advertised route
    thrift::AdvertisedRoute route;
    route.set_key(bestPrefixType);
    route.set_route(*postPolicyTPrefixEntry);
    if (not hitPolicyName.empty()) {
      route.set_hitPolicy(hitPolicyName);
    }
    routes.emplace_back(std::move(route));
    return;
  }

  if (routeFilterType == thrift::RouteFilterType::REJECTED_ON_ADVERTISE and
      not postPolicyTPrefixEntry) {
    // add post filter rejected route
    thrift::AdvertisedRoute route;
    route.set_key(bestPrefixType);
    route.set_route(*prePolicyTPrefixEntry);
    route.set_hitPolicy(hitPolicyName);
    routes.emplace_back(std::move(route));
  }
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
    const auto& type = *entry.tPrefixEntry->type_ref();
    const auto& prefixCidr = entry.network;

    // ATTN: create new folly::CIDRNetwork -> typeToPrefixes
    //       mapping if it is new prefix. `[]` operator is
    //       used intentionally.
    auto [it, inserted] = prefixMap_[prefixCidr].emplace(type, entry);

    if (not inserted) {
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
    const auto& type = *prefixEntry.type_ref();
    const auto& prefixCidr = toIPNetwork(*prefixEntry.prefix_ref());

    // iterator usage to avoid multiple times of map access
    auto typeIt = prefixMap_.find(prefixCidr);

    // ONLY populate changed collection when successfully erased key
    if (typeIt != prefixMap_.end() and typeIt->second.erase(type)) {
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
    const auto& type = *prefixEntry.tPrefixEntry->type_ref();

    // iterator usage to avoid multiple times of map access
    auto typeIt = prefixMap_.find(prefixEntry.network);

    // ONLY populate changed collection when successfully erased key
    if (typeIt != prefixMap_.end() and typeIt->second.erase(type)) {
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
  VLOG(1) << "Syncing prefixes of type " << toString(type);
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
    CHECK(type == *entry.type_ref());
    toRemoveSet.erase(toIPNetwork(*entry.prefix_ref()));
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
  if (not inserted) {
    return;
  }

  for (auto& [network, route] : originatedPrefixDb_) {
    // folly::CIDRNetwork.first -> IPAddress
    // folly::CIDRNetwork.second -> cidr length
    if (not prefix.first.inSubnet(network.first, network.second)) {
      continue;
    }

    VLOG(1) << "[Route Origination] Adding supporting route "
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

    VLOG(1) << "[Route Origination] Removing supporting route "
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

  for (auto& [network, route] : originatedPrefixDb_) {
    if (route.shouldAdvertise()) {
      route.isAdvertised = true; // mark as advertised
      advertisedPrefixes.emplace_back(
          std::make_shared<thrift::PrefixEntry>(
              route.unicastEntry.bestPrefixEntry),
          allAreaIds());
      if (route.originatedPrefix.install_to_fib_ref().has_value() &&
          *route.originatedPrefix.install_to_fib_ref()) {
        advertisedPrefixes.back().nexthops = route.unicastEntry.nexthops;
      }
      LOG(INFO) << "[Route Origination] Advertising originated route "
                << folly::IPAddress::networkToString(network);
    }

    if (route.shouldWithdraw()) {
      route.isAdvertised = false; // mark as withdrawn
      withdrawnPrefixes.emplace_back(
          createPrefixEntry(toIpPrefix(network), thrift::PrefixType::CONFIG));

      VLOG(1) << "[Route Origination] Withdrawing originated route "
              << folly::IPAddress::networkToString(network);
    }
  }
  // advertise originated config routes to KvStore
  advertisePrefixesImpl(advertisedPrefixes);
  withdrawPrefixesImpl(withdrawnPrefixes);
}

void
PrefixManager::processFibRouteUpdates(DecisionRouteUpdate&& fibRouteUpdate) {
  if (config_->getConfig().get_enable_fib_ack()) {
    // Store programmed label/unicast routes info if FIB-ACK feature is enabled.
    storeProgrammedRoutes(fibRouteUpdate);
  }

  // Re-advertise prefixes received from one area to other areas.
  redistributePrefixesAcrossAreas(fibRouteUpdate);
}

void
PrefixManager::storeProgrammedRoutes(
    const DecisionRouteUpdate& fibRouteUpdates) {
  // In case of full sync, reset previous stored programmed routes.
  if (fibRouteUpdates.type == DecisionRouteUpdate::FULL_SYNC) {
    if (uninitializedPrefixTypes_.erase(thrift::PrefixType::RIB)) {
      LOG(INFO) << "[Initialization] Received initial RIB routes.";
      triggerInitialPrefixDbSync();
    }

    // Adding all previously programmed routes as pending updates.
    for (const auto& deletedLabel : programmedLabels_) {
      pendingUpdates_.addLabelChange(deletedLabel);
    }
    for (const auto& deletedPrefix : programmedPrefixes_) {
      pendingUpdates_.addPrefixChange(deletedPrefix);
    }
    programmedLabels_.clear();
    programmedPrefixes_.clear();
  }

  // Record MPLS routes from OpenR/Fib.
  for (auto& [label, _] : fibRouteUpdates.mplsRoutesToUpdate) {
    if (programmedLabels_.insert(label).second /*inserted*/) {
      pendingUpdates_.addLabelChange(label);
    }
  }
  for (auto& deletedLabel : fibRouteUpdates.mplsRoutesToDelete) {
    if (programmedLabels_.erase(deletedLabel) /*erased*/) {
      pendingUpdates_.addLabelChange(deletedLabel);
    }
  }
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

namespace {
void
resetNonTransitiveAttrs(thrift::PrefixEntry& prefixEntry) {
  // Reset non-transitive attributes which cannot be redistributed across
  // areas. Ref:
  // https://openr.readthedocs.io/Operator_Guide/RouteRepresentation.html.
  prefixEntry.forwardingAlgorithm_ref() =
      thrift::PrefixForwardingAlgorithm::SP_ECMP;
  prefixEntry.forwardingType_ref() = thrift::PrefixForwardingType::IP;
  prefixEntry.minNexthop_ref().reset();
  prefixEntry.prependLabel_ref().reset();
}
} // namespace

void
PrefixManager::redistributePrefixesAcrossAreas(
    DecisionRouteUpdate& fibRouteUpdate) {
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

    if (*prefixEntry.type_ref() == thrift::PrefixType::CONFIG) {
      // Skip local-originated prefix as it won't be considered as
      // part of its own supporting routes.
      auto originatedPrefixIt = originatedPrefixDb_.find(prefix);
      if (originatedPrefixIt != originatedPrefixDb_.end()) {
        continue;
      }
    }

    // Update interested mutable transitive attributes.
    //
    // For OpenR route representation, referring to
    // https://openr.readthedocs.io/Operator_Guide/RouteRepresentation.html
    // 1. append area stack
    prefixEntry.area_stack_ref()->emplace_back(route.bestArea);
    // 2. increase distance by 1
    ++(*prefixEntry.metrics_ref()->distance_ref());
    // 3. normalize to RIB routes
    prefixEntry.type_ref() = thrift::PrefixType::RIB;

    // Reset non-transitive attributes before redistribution across areas.
    resetNonTransitiveAttrs(prefixEntry);

    // Populate routes to be advertised to KvStore
    auto dstAreas = allAreaIds();
    for (const auto& nh : route.nexthops) {
      if (nh.area_ref().has_value()) {
        dstAreas.erase(*nh.area_ref());
      }
    }
    advertisedPrefixes.emplace_back(
        std::make_shared<thrift::PrefixEntry>(std::move(prefixEntry)),
        std::move(dstAreas));

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
