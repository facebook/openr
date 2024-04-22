/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/io/async/SSLContext.h>
#include <folly/logging/xlog.h>

#include <openr/common/Constants.h>
#include <openr/common/EventLogger.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace fb303 = facebook::fb303;

namespace openr {

template <class ClientType>
KvStore<ClientType>::KvStore(
    // initializers for immutable state
    messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQueue,
    messaging::RQueue<PeerEvent> peerUpdatesQueue,
    messaging::RQueue<KeyValueRequest> kvRequestQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    const std::unordered_set<std::string>& areaIds,
    const thrift::KvStoreConfig& kvStoreConfig)
    : kvParams_(kvStoreConfig, kvStoreUpdatesQueue, logSampleQueue) {
  // Schedule periodic timer for counters submission
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    for (auto& [key, val] : getGlobalCounters()) {
      fb303::fbData->setCounter(key, val);
    }
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);

  // Get optional ip_tos from the config
  kvParams_.maybeIpTos = kvStoreConfig.ip_tos().to_optional();
  if (kvParams_.maybeIpTos.has_value()) {
    XLOG(INFO) << fmt::format(
        "Set IP_TOS: {} for node: {}",
        kvParams_.maybeIpTos.value(),
        *kvStoreConfig.node_name());
  }
  if (*kvStoreConfig.sync_initial_backoff_ms() <= 0) {
    XLOG(INFO) << fmt::format(
        "non-zero sync initial backoff ms {}, re-setting to {}",
        *kvStoreConfig.sync_initial_backoff_ms(),
        std::chrono::milliseconds(Constants::kKvstoreSyncInitialBackoff)
            .count());

    kvParams_.syncInitialBackoff = Constants::kKvstoreSyncInitialBackoff;
  } else {
    kvParams_.syncInitialBackoff =
        std::chrono::milliseconds(*kvStoreConfig.sync_initial_backoff_ms());
  }

  if (*kvStoreConfig.sync_max_backoff_ms() <
      std::chrono::milliseconds(kvParams_.syncInitialBackoff).count()) {
    if (kvParams_.syncInitialBackoff < Constants::kKvstoreSyncMaxBackoff) {
      kvParams_.syncMaxBackoff = Constants::kKvstoreSyncMaxBackoff;
    } else {
      // to be tuned if this case is of interest
      kvParams_.syncMaxBackoff = (kvParams_.syncInitialBackoff * 2);
    }

    XLOG(INFO) << fmt::format(
        "sync max backoff ms {} less than initial backoff, re-setting to {}",
        *kvStoreConfig.sync_max_backoff_ms(),
        kvParams_.syncMaxBackoff.count());
  } else {
    kvParams_.syncMaxBackoff =
        std::chrono::milliseconds(*kvStoreConfig.sync_max_backoff_ms());
  }

  XLOG(INFO) << fmt::format(
      "Initial backoff {} and Max backoff {}",
      kvParams_.syncInitialBackoff.count(),
      kvParams_.syncMaxBackoff.count());

  {
    auto fiber = addFiberTaskFuture(
        [q = std::move(peerUpdatesQueue), this]() mutable noexcept {
          XLOG(DBG1) << "Starting peer-updates task";
          while (true) {
            auto maybePeerUpdate = q.get(); // perform read
            if (maybePeerUpdate.hasError()) {
              break;
            }
            try {
              processPeerUpdates(std::move(maybePeerUpdate).value());
            } catch (const std::exception& ex) {
              XLOG(ERR) << "Failed to process peer request. Exception: "
                        << ex.what();
            }
          }
          XLOG(DBG1) << "[Exit] Peer-updates task finished";
        });
    kvStoreWorkers_.emplace_back(std::move(fiber));
  }

  {
    auto fiber = addFiberTaskFuture(
        [kvQueue = std::move(kvRequestQueue), this]() mutable noexcept {
          XLOG(DBG1) << "Starting kv-requests task";
          while (true) {
            auto maybeKvRequest = kvQueue.get(); // perform read
            if (maybeKvRequest.hasError()) {
              break;
            }
            try {
              processKeyValueRequest(std::move(maybeKvRequest).value());
            } catch (const std::exception& ex) {
              XLOG(ERR) << "Failed to process key-value request. Exception: "
                        << ex.what();
            }
          }
          XLOG(DBG1) << "[Exit] Kv-requests task finished";
        });
    kvStoreWorkers_.emplace_back(std::move(fiber));
  }

  initGlobalCounters();

  // create KvStoreDb instances
  for (auto const& area : areaIds) {
    kvStoreDb_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(area),
        std::forward_as_tuple(
            this,
            kvParams_,
            area,
            *kvStoreConfig.node_name(),
            std::bind(&KvStore::initialKvStoreDbSynced, this)));
  }
}

template <class ClientType>
void
KvStore<ClientType>::stop() {
  const auto num = kvStoreDb_.size();

  XLOG(DBG1) << fmt::format(
      "[Exit] Stopping {} kvStoreDbs with {} tasks.", num, getFiberTaskNum());

  /*
   * NOTE: mark each individual kvStoreDb terminated and cleanup:
   *  - AsyncThrottle
   *  - AsyncTimeout
   *  - release all KvStorePeer and thrift client resource
   *  -etc.
   */
  for (auto& [area, kvDb] : kvStoreDb_) {
    kvDb.stop();
  }

  XLOG(DBG1) << fmt::format("[Exit] Successfully stop {} kvStoreDbs", num);

  // waits for all child fibers to complete
  folly::collectAll(kvStoreWorkers_.begin(), kvStoreWorkers_.end()).get();

  // NOTE: folly::AsyncTimeout and AsyncThrottle must be tear-down in evb loop
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this]() { counterUpdateTimer_.reset(); });

  // Invoke stop method of super class
  OpenrEventBase::stop();

  XLOG(DBG1) << fmt::format(
      "[Exit] Successfully stopped KvStore evb along with {} kvStoreDb.", num);
}

template <class ClientType>
KvStoreDb<ClientType>&
KvStore<ClientType>::getAreaDbOrThrow(
    std::string const& areaId, std::string const& caller) {
  auto search = kvStoreDb_.find(areaId);
  if (kvStoreDb_.end() == search) {
    XLOG(WARNING) << fmt::format(
        "Area {} requested but not configured for this node.", areaId);

    // ATTN: AreaId "0" a special area that is treated as the wildcard area.
    // We will still do FULL_SYNC if:
    //  1) We are ONLY configured with single areaId "0";
    //  2) We are ONLY configured with single areaId(may NOT be "0") and
    //     with areaId "0" from peer's sync request;
    const auto defaultArea = Constants::kDefaultArea.toString();
    if (kvStoreDb_.size() == 1 and
        (kvStoreDb_.count(defaultArea) or areaId == defaultArea)) {
      XLOG(INFO) << fmt::format(
          "Falling back to my single area: {}", kvStoreDb_.begin()->first);
      fb303::fbData->addStatValue(
          fmt::format("kvstore.default_area_compatibility.{}", caller),
          1,
          fb303::COUNT);
      return kvStoreDb_.begin()->second;
    } else {
      throw thrift::KvStoreError(fmt::format("Invalid area: {}", areaId));
    }
  }
  return search->second;
}

template <class ClientType>
void
KvStore<ClientType>::processKeyValueRequest(KeyValueRequest&& kvRequest) {
  // get area across different variants of KeyValueRequest
  const auto& area = std::visit(
      [](auto&& request) -> AreaId { return request.getArea(); }, kvRequest);

  try {
    auto& kvStoreDb = getAreaDbOrThrow(area, "processKeyValueRequest");
    if (auto pPersistKvRequest =
            std::get_if<PersistKeyValueRequest>(&kvRequest)) {
      kvStoreDb.persistSelfOriginatedKey(
          pPersistKvRequest->getKey(), pPersistKvRequest->getValue());
    } else if (
        auto pSetKvRequest = std::get_if<SetKeyValueRequest>(&kvRequest)) {
      kvStoreDb.setSelfOriginatedKey(
          pSetKvRequest->getKey(),
          pSetKvRequest->getValue(),
          pSetKvRequest->getVersion());
    } else if (
        auto pClearKvRequest = std::get_if<ClearKeyValueRequest>(&kvRequest)) {
      if (pClearKvRequest->getSetValue()) {
        kvStoreDb.unsetSelfOriginatedKey(
            pClearKvRequest->getKey(), pClearKvRequest->getValue());
      } else {
        kvStoreDb.eraseSelfOriginatedKey(pClearKvRequest->getKey());
      }
    } else {
      XLOG(ERR)
          << "Error processing key value request. Request type not recognized.";
    }
  } catch (thrift::KvStoreError const&) {
    XLOG(ERR) << " Failed to find area " << area.t << " in kvStoreDb_.";
  }
}

template <class ClientType>
messaging::RQueue<KvStorePublication>
KvStore<ClientType>::getKvStoreUpdatesReader() {
  return kvParams_.kvStoreUpdatesQueue.getReader();
}

template <class ClientType>
void
KvStore<ClientType>::processPeerUpdates(PeerEvent&& event) {
  for (const auto& [area, areaPeerEvent] : event) {
    // Event can contain peerAdd/peerDel simultaneously
    if (not areaPeerEvent.peersToAdd.empty()) {
      semifuture_addUpdateKvStorePeers(area, areaPeerEvent.peersToAdd).get();
    }
    if (not areaPeerEvent.peersToDel.empty()) {
      semifuture_deleteKvStorePeers(area, areaPeerEvent.peersToDel).get();
    }
  }

  if ((not initialSyncSignalSent_)) {
    // In OpenR initialization process, first PeerEvent publishment from
    // LinkMonitor includes peers in all areas. However, KvStore could receive
    // empty peers in one configured area in following scenarios,
    // - The device is running in standalone mode,
    // - The configured area just spawns without any peer yet.
    // In order to make KvStore converge in initialization process, KvStoreDb
    // with no peers in the area is treated as syncing completed. Otherwise,
    // 'initialKvStoreDbSynced()' will not publish kvStoreSynced signal, and
    // downstream modules cannot proceed to complete initialization.
    for (auto& [area, kvStoreDb] : kvStoreDb_) {
      if (kvStoreDb.getPeerCnt() != 0) {
        continue;
      }
      XLOG(INFO)
          << fmt::format("[Initialization] Received 0 peers in area {}.", area);
      kvStoreDb.processInitializationEvent();
    }
  }
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStore<ClientType>::semifuture_getKvStoreKeyVals(
    std::string area, thrift::KeyGetParams keyGetParams) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keyGetParams = std::move(keyGetParams),
                        area]() mutable {
    XLOG(DBG3) << "Get key requested for AREA: " << area;
    try {
      auto& kvStoreDb = getAreaDbOrThrow(area, "getKvStoreKeyVals");

      auto thriftPub = kvStoreDb.getKeyVals(*keyGetParams.keys());
      updatePublicationTtl(
          kvStoreDb.getTtlCountdownQueue(),
          kvParams_.ttlDecr,
          thriftPub,
          false);

      p.setValue(std::make_unique<thrift::Publication>(std::move(thriftPub)));
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<SelfOriginatedKeyVals>>
KvStore<ClientType>::semifuture_dumpKvStoreSelfOriginatedKeys(
    std::string area) {
  folly::Promise<std::unique_ptr<SelfOriginatedKeyVals>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), area]() mutable {
    XLOG(DBG3) << "Dump self originated key-vals for AREA: " << area;
    try {
      auto& kvStoreDb =
          getAreaDbOrThrow(area, "semifuture_dumpKvStoreSelfOriginatedKeys");
      // track self origin key-val dump calls
      fb303::fbData->addStatValue(
          "kvstore.cmd_self_originated_key_dump", 1, fb303::COUNT);

      auto keyVals = kvStoreDb.getSelfOriginatedKeyVals();
      p.setValue(std::make_unique<SelfOriginatedKeyVals>(keyVals));
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
std::unique_ptr<std::vector<thrift::Publication>>
KvStore<ClientType>::dumpKvStoreKeysImpl(
    thrift::KeyDumpParams keyDumpParams, std::set<std::string> selectAreas) {
  const auto areaStr =
      (selectAreas.empty()
           ? "default areas."
           : fmt::format("areas: {}", folly::join(", ", selectAreas)));
  const auto senderStr =
      (keyDumpParams.senderId().has_value() ? keyDumpParams.senderId().value()
                                            : "");
  XLOG(DBG3) << fmt::format(
      "Dump all keys requested for {}, by sender: {}", areaStr, senderStr);

  auto result = std::make_unique<std::vector<thrift::Publication>>();
  for (auto& area : selectAreas) {
    try {
      auto& kvStoreDb = getAreaDbOrThrow(area, "dumpKvStoreKeys");
      fb303::fbData->addStatValue("kvstore.cmd_key_dump", 1, fb303::COUNT);

      // KvStoreFilters contains `thrift::FilterOperator`
      // Default to thrift::FilterOperator::OR
      thrift::FilterOperator oper = thrift::FilterOperator::OR;
      if (keyDumpParams.oper().has_value()) {
        oper = *keyDumpParams.oper();
      }

      thrift::Publication thriftPub;
      try {
        const auto keyPrefixMatch = KvStoreFilters(
            *keyDumpParams.keys(), *keyDumpParams.originatorIds(), oper);
        thriftPub = dumpAllWithFilters(
            area,
            kvStoreDb.getKeyValueMap(),
            keyPrefixMatch,
            *keyDumpParams.doNotPublishValue());
      } catch (RegexSetException const& err) {
        XLOG(ERR) << fmt::format(
            "Fail to create KvStoreFilters with exception: {}. Dump without filter",
            folly::exceptionStr(err));
        const auto keyPrefixMatch =
            KvStoreFilters({}, *keyDumpParams.originatorIds(), oper);
        thriftPub = dumpAllWithFilters(
            area,
            kvStoreDb.getKeyValueMap(),
            keyPrefixMatch,
            *keyDumpParams.doNotPublishValue());
      }

      if (keyDumpParams.keyValHashes().has_value()) {
        thriftPub = dumpDifference(
            area, *thriftPub.keyVals(), keyDumpParams.keyValHashes().value());
      }
      updatePublicationTtl(
          kvStoreDb.getTtlCountdownQueue(), kvParams_.ttlDecr, thriftPub);

      if (keyDumpParams.keyValHashes().has_value() and
          (*keyDumpParams.keys()).empty()) {
        // This usually comes from neighbor nodes
        size_t numMissingKeys = 0;
        if (thriftPub.tobeUpdatedKeys().has_value()) {
          numMissingKeys = thriftPub.tobeUpdatedKeys()->size();
        }
        XLOG(INFO) << fmt::format(
            "[Thrift Sync] Processed full-sync request with {}  keyValHashes item(s). Sending {} key-vals and {} missing keys.",
            keyDumpParams.keyValHashes().value().size(),
            thriftPub.keyVals()->size(),
            numMissingKeys);
      }
      result->push_back(std::move(thriftPub));
    } catch (thrift::KvStoreError const&) {
      XLOG(ERR) << fmt::format("Failed to find area {} in kvStoreDb_", area);
    }
  }
  return result;
}

template <class ClientType>
std::vector<thrift::KvStoreAreaSummary>
KvStore<ClientType>::getKvStoreAreaSummaryImpl(
    std::set<std::string> selectAreas) {
  const auto area =
      (selectAreas.empty()
           ? "all areas."
           : fmt::format("areas: {}.", folly::join(", ", selectAreas)));
  XLOG(INFO) << fmt::format("KvStore Summary requested for {}", area);

  std::vector<thrift::KvStoreAreaSummary> result;
  for (auto& [area, kvStoreDb] : kvStoreDb_) {
    thrift::KvStoreAreaSummary areaSummary;

    areaSummary.area() = area;
    auto kvDbCounters = kvStoreDb.getCounters();
    areaSummary.keyValsCount() = kvDbCounters["kvstore.num_keys"];
    areaSummary.peersMap() = kvStoreDb.dumpPeers();
    areaSummary.keyValsBytes() = kvStoreDb.getKeyValsSize();

    result.emplace_back(std::move(areaSummary));
  }
  return result;
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<std::vector<thrift::Publication>>>
KvStore<ClientType>::semifuture_dumpKvStoreKeys(
    thrift::KeyDumpParams keyDumpParams, std::set<std::string> selectAreas) {
  folly::Promise<std::unique_ptr<std::vector<thrift::Publication>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        selectAreas = std::move(selectAreas),
                        keyDumpParams = std::move(keyDumpParams)]() mutable {
    auto result =
        dumpKvStoreKeysImpl(std::move(keyDumpParams), std::move(selectAreas));
    p.setValue(std::move(result));
  });
  return sf;
}

template <class ClientType>
thrift::Publication
KvStore<ClientType>::dumpKvStoreHashesImpl(
    std::string area, thrift::KeyDumpParams keyDumpParams) {
  const auto senderStr =
      (keyDumpParams.senderId().has_value() ? keyDumpParams.senderId().value()
                                            : "");
  XLOG(DBG3) << fmt::format(
      "Dump all hashes requested for AREA: {}, by sender: {}", area, senderStr);
  auto& kvStoreDb = getAreaDbOrThrow(area, "dumpKvStoreHashesImpl");
  fb303::fbData->addStatValue("kvstore.cmd_hash_dump", 1, fb303::COUNT);
  std::set<std::string> originatorIdList{};
  KvStoreFilters kvFilters{*keyDumpParams.keys(), std::move(originatorIdList)};
  auto thriftPub =
      dumpHashWithFilters(area, kvStoreDb.getKeyValueMap(), kvFilters);
  updatePublicationTtl(
      kvStoreDb.getTtlCountdownQueue(), kvParams_.ttlDecr, thriftPub);
  return thriftPub;
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStore<ClientType>::semifuture_dumpKvStoreHashes(
    std::string area, thrift::KeyDumpParams keyDumpParams) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keyDumpParams = std::move(keyDumpParams),
                        area]() mutable {
    try {
      auto result =
          dumpKvStoreHashesImpl(std::move(area), std::move(keyDumpParams));
      p.setValue(std::make_unique<thrift::Publication>(std::move(result)));
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
folly::SemiFuture<folly::Unit>
KvStore<ClientType>::semifuture_setKvStoreKeyVals(
    std::string area, thrift::KeySetParams keySetParams) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keySetParams = std::move(keySetParams),
                        area]() mutable {
    XLOG(DBG3) << fmt::format(
        "Set key requested for AREA: {}, by sender: {}, at time: {}",
        area,
        (keySetParams.senderId().has_value() ? keySetParams.senderId().value()
                                             : ""),
        (keySetParams.timestamp_ms().has_value()
             ? folly::to<std::string>(keySetParams.timestamp_ms().value())
             : ""));
    try {
      auto& kvStoreDb = getAreaDbOrThrow(area, "setKvStoreKeyVals");
      kvStoreDb.setKeyVals(std::move(keySetParams), false /* remote update */);
      // ready to return
      p.setValue();
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::SetKeyValsResult>>
KvStore<ClientType>::semifuture_setKvStoreKeyValues(
    std::string area, thrift::KeySetParams keySetParams) {
  folly::Promise<std::unique_ptr<thrift::SetKeyValsResult>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keySetParams = std::move(keySetParams),
                        area]() mutable {
    XLOG(DBG3) << fmt::format(
        "Set key requested for AREA: {}, by sender: {}, at time: {}",
        area,
        (keySetParams.senderId().has_value() ? keySetParams.senderId().value()
                                             : ""),
        (keySetParams.timestamp_ms().has_value()
             ? folly::to<std::string>(keySetParams.timestamp_ms().value())
             : ""));
    try {
      auto& kvStoreDb = getAreaDbOrThrow(area, "setKvStoreKeyVals");
      auto r = kvStoreDb.setKeyVals(
          std::move(keySetParams), false /* remote update */);
      // ready to return
      p.setValue(std::make_unique<thrift::SetKeyValsResult>(std::move(r)));
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<bool>>
KvStore<ClientType>::semifuture_injectThriftFailure(
    std::string area, std::string peerName) {
  folly::Promise<std::unique_ptr<bool>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), peerName = std::move(peerName), area]() mutable {
        try {
          bool r = true;
          auto& kvStoreDb = getAreaDbOrThrow(area, "disconnectPeer");
          kvStoreDb.processThriftFailure(
              peerName,
              "injected thrift failure",
              std::chrono::milliseconds(500)); // arbitrary timeout
          p.setValue(std::make_unique<bool>(std::move(r)));
        } catch (thrift::KvStoreError const& e) {
          p.setException(e);
        }
      });
  return sf;
}

template <class ClientType>
folly::SemiFuture<std::optional<thrift::KvStorePeerState>>
KvStore<ClientType>::semifuture_getKvStorePeerState(
    std::string const& area, std::string const& peerName) {
  folly::Promise<std::optional<thrift::KvStorePeerState>> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(promise), peerName, area]() mutable {
        try {
          p.setValue(getAreaDbOrThrow(area, "semifuture_getKvStorePeerState")
                         .getCurrentState(peerName));
        } catch (thrift::KvStoreError const& e) {
          p.setException(e);
        }
      });
  return sf;
}

/**
 * @brief utility function to get epoch time of a given peer, from a given area,
 * when it has seen last state change
 *
 * @param area      name of area kvstore peer belongs
 * @param peerName  name of the kvstore peer
 *
 * @return folly::SemiFuture<int64_t> epoch time in ms
 */
template <class ClientType>
folly::SemiFuture<int64_t>
KvStore<ClientType>::semifuture_getKvStorePeerStateEpochTimeMs(
    std::string const& area, std::string const& peerName) {
  folly::Promise<int64_t> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(promise), peerName, area]() mutable {
        try {
          p.setValue(getAreaDbOrThrow(
                         area, "semifuture_getKvStorePeerStateEpochTimeMs")
                         .getCurrentStateEpochTimeMs(peerName));
        } catch (thrift::KvStoreError const& e) {
          p.setException(e);
        }
      });
  return sf;
}

/**
 * @brief utility function to get elapsed time of a given peer, from a given
 * area, since last state peer has been in
 *
 * @param area      name of area kvstore peer belongs
 * @param peerName  name of the kvstore peer
 *
 * @return folly::SemiFuture<int64_t> elapsed time in ms
 */
template <class ClientType>
folly::SemiFuture<int64_t>
KvStore<ClientType>::semifuture_getKvStorePeerStateElapsedTimeMs(
    std::string const& area, std::string const& peerName) {
  folly::Promise<int64_t> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(promise), peerName, area]() mutable {
        try {
          p.setValue(getAreaDbOrThrow(
                         area, "semifuture_getKvStorePeerStateElapsedTimeMs")
                         .getCurrentStateElapsedTimeMs(peerName));
        } catch (thrift::KvStoreError const& e) {
          p.setException(e);
        }
      });
  return sf;
}

/**
 * @brief utility function to get number of flaps to a specific peer (either
 * from INITIALIZED state or to INITIALIZED state)
 *
 * @param area      name of area kvstore peer belongs
 * @param peerName  name of the kvstore peer
 *
 * @return folly::SemiFuture<int32_t> number of flaps
 */
template <class ClientType>
folly::SemiFuture<int32_t>
KvStore<ClientType>::semifuture_getKvStorePeerFlaps(
    std::string const& area, std::string const& peerName) {
  folly::Promise<int32_t> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(promise), peerName, area]() mutable {
        try {
          p.setValue(getAreaDbOrThrow(area, "semifuture_getKvStorePeerFlaps")
                         .getCurrentFlaps(peerName));
        } catch (thrift::KvStoreError const& e) {
          p.setException(e);
        }
      });
  return sf;
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
KvStore<ClientType>::semifuture_getKvStorePeers(std::string area) {
  folly::Promise<std::unique_ptr<thrift::PeersMap>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), area]() mutable {
    XLOG(DBG2) << fmt::format("Peer dump requested for AREA: {}", area);
    try {
      p.setValue(std::make_unique<thrift::PeersMap>(
          getAreaDbOrThrow(area, "semifuture_getKvStorePeers").dumpPeers()));
      fb303::fbData->addStatValue("kvstore.cmd_peer_dump", 1, fb303::COUNT);
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>>
KvStore<ClientType>::semifuture_getKvStoreAreaSummaryInternal(
    std::set<std::string> selectAreas) {
  folly::Promise<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(p), selectAreas = std::move(selectAreas)]() mutable {
        auto result = getKvStoreAreaSummaryImpl(std::move(selectAreas));
        p.setValue(std::make_unique<std::vector<thrift::KvStoreAreaSummary>>(
            std::move(result)));
      });
  return sf;
}

template <class ClientType>
folly::SemiFuture<folly::Unit>
KvStore<ClientType>::semifuture_addUpdateKvStorePeers(
    std::string area, thrift::PeersMap peersToAdd) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        peersToAdd = std::move(peersToAdd),
                        area]() mutable {
    try {
      auto str = folly::gen::from(peersToAdd) | folly::gen::get<0>() |
          folly::gen::as<std::vector<std::string>>();

      XLOG(INFO) << "Peer addition for: [" << folly::join(",", str)
                 << "] in area: " << area;
      auto& kvStoreDb =
          getAreaDbOrThrow(area, "semifuture_addUpdateKvStorePeers");
      if (peersToAdd.empty()) {
        p.setException(thrift::KvStoreError(
            "Empty peerNames from peer-add request, ignoring"));
      } else {
        fb303::fbData->addStatValue("kvstore.cmd_peer_add", 1, fb303::COUNT);
        kvStoreDb.addThriftPeers(peersToAdd);
        p.setValue();
      }
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
folly::SemiFuture<folly::Unit>
KvStore<ClientType>::semifuture_deleteKvStorePeers(
    std::string area, std::vector<std::string> peersToDel) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        peersToDel = std::move(peersToDel),
                        area]() mutable {
    XLOG(INFO) << "Peer deletion for: [" << folly::join(",", peersToDel)
               << "] in area: " << area;
    try {
      auto& kvStoreDb = getAreaDbOrThrow(area, "semifuture_deleteKvStorePeers");
      if (peersToDel.empty()) {
        p.setException(thrift::KvStoreError(
            "Empty peerNames from peer-del request, ignoring"));
      } else {
        fb303::fbData->addStatValue("kvstore.cmd_per_del", 1, fb303::COUNT);
        kvStoreDb.delThriftPeers(peersToDel);
        p.setValue();
      }
    } catch (thrift::KvStoreError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

template <class ClientType>
void
KvStore<ClientType>::initialKvStoreDbSynced() {
  for (auto& [_, kvStoreDb] : kvStoreDb_) {
    if (not kvStoreDb.getInitialSyncedWithPeers()) {
      return;
    }
  }

  if (not initialSyncSignalSent_) {
    // Publish KvStore synced signal.
    kvParams_.kvStoreUpdatesQueue.push(
        thrift::InitializationEvent::KVSTORE_SYNCED);
    initialSyncSignalSent_ = true;
    logInitializationEvent(
        "KvStore",
        thrift::InitializationEvent::KVSTORE_SYNCED,
        fmt::format(
            "KvStoreDb sync is completed in all {} areas.", kvStoreDb_.size()));
  }
}

template <class ClientType>
folly::SemiFuture<std::map<std::string, int64_t>>
KvStore<ClientType>::semifuture_getCounters() {
  auto [p, f] = folly::makePromiseContract<std::map<std::string, int64_t>>();
  runInEventBaseThread([this, p = std::move(p)]() mutable { //
    p.setValue(getGlobalCounters());
  });
  return std::move(f);
}

template <class ClientType>
std::map<std::string, int64_t>
KvStore<ClientType>::getGlobalCounters() const {
  std::map<std::string, int64_t> flatCounters;
  for (auto& [_, kvDb] : kvStoreDb_) {
    auto kvDbCounters = kvDb.getCounters();
    // add up counters for same key from all kvStoreDb instances
    std::for_each(
        kvDbCounters.begin(),
        kvDbCounters.end(),
        [&](const std::pair<const std::string, int64_t>& kvDbCounter) {
          flatCounters[kvDbCounter.first] += kvDbCounter.second;
        });
  }
  return flatCounters;
}

template <class ClientType>
void
KvStore<ClientType>::initGlobalCounters() {
  /*
   * [KvStore Thrift Sync]
   *
   * Initialize counters for KvStore thrift I/O monitoring, which includes:
   *  1) success/failure stats for different types of messages;
   *  2) time elapsed for different types of messages;
   *  3) etc.
   */
  fb303::fbData->addStatExportType(
      "kvstore.thrift.secure_client", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.plaintext_client.fallback", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.plaintext_client", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_client_connection_failure", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.semifuture_setKvStoreKeyVals.secure_client.failure",
      fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.semifuture_getKvStoreKeyValsFilteredArea.secure_client.failure",
      fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.co_getKvStoreKeyValsFilteredArea.secure_client.failure",
      fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_full_sync", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_full_sync_success", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_full_sync_failure", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_pub", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_pub_success", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_pub_failure", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_finalized_sync", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_finalized_sync_success", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_finalized_sync_failure", fb303::COUNT);

  fb303::fbData->addStatExportType(
      "kvstore.thrift.full_sync_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.flood_pub_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.finalized_sync_duration_ms", fb303::AVG);

  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_missing_keys", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_keyvals_update", fb303::SUM);

  // Initialize stats keys
  fb303::fbData->addStatExportType("kvstore.expired_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.rate_limit_keys", fb303::AVG);
  fb303::fbData->addStatExportType("kvstore.rate_limit_suppress", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.looped_publications", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.received_redundant_publications", fb303::COUNT);

  /*
   * [KvStore Monitoring]
   */

  // Cmd statistics monitoring
  fb303::fbData->addStatExportType("kvstore.cmd_hash_dump", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.cmd_self_originated_key_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_get", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_set", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_peer_add", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_peer_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_per_del", fb303::COUNT);

  // KvStore publications monitoring
  fb303::fbData->addStatExportType(
      "kvstore.received_publications", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.received_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.updated_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.num_conflict_version_key", fb303::SUM);
}

// static util function to fetch peers by state
template <class ClientType>
std::vector<std::string>
KvStoreDb<ClientType>::getPeersByState(thrift::KvStorePeerState state) {
  std::vector<std::string> res;
  for (auto const& [_, peer] : thriftPeers_) {
    if (*peer.peerSpec.state() == state) {
      res.emplace_back(peer.nodeName);
    }
  }
  return res;
}

// static util function to log state transition
template <class ClientType>
void
KvStoreDb<ClientType>::logStateTransition(
    std::string const& peerName,
    thrift::KvStorePeerState oldState,
    thrift::KvStorePeerState newState) {
  SYSLOG(INFO)
      << EventTag() << AreaTag()
      << fmt::format(
             "State change: [{}] -> [{}] for peer: {}",
             apache::thrift::util::enumNameSafe<thrift::KvStorePeerState>(
                 oldState),
             apache::thrift::util::enumNameSafe<thrift::KvStorePeerState>(
                 newState),
             peerName);
}

// static util function to fetch current peer state
template <class ClientType>
std::optional<thrift::KvStorePeerState>
KvStoreDb<ClientType>::getCurrentState(std::string const& peerName) {
  auto thriftPeerIt = thriftPeers_.find(peerName);
  if (thriftPeerIt != thriftPeers_.end()) {
    return *thriftPeerIt->second.peerSpec.state();
  }
  return std::nullopt;
}

/**
 * @brief static util function to get epoch time
 *
 * @return int64_t  elapsed time in ms
 */
inline int64_t
getTimeSinceEpochMs(void) {
  auto curTime = std::chrono::steady_clock::now();
  return (std::chrono::duration_cast<std::chrono::milliseconds>(
              curTime.time_since_epoch())
              .count());
}

/**
 * @brief static util function to fetch current peer state epoch time
 *
 * @param peerName  name of the kvstore peer
 *
 * @return int64_t  epoch time in ms
 */
template <class ClientType>
int64_t
KvStoreDb<ClientType>::getCurrentStateEpochTimeMs(std::string const& peerName) {
  auto thriftPeerIt = thriftPeers_.find(peerName);
  if (thriftPeerIt != thriftPeers_.end()) {
    return *thriftPeerIt->second.peerSpec.stateEpochTimeMs();
  }
  return 0;
}

/**
 * @brief static util function to fetch current peer state elapsed time
 *
 * @param peerName  name of the kvstore peer
 *
 * @return int64_t  elapsed time in ms
 */
template <class ClientType>
int64_t
KvStoreDb<ClientType>::getCurrentStateElapsedTimeMs(
    std::string const& peerName) {
  auto thriftPeerIt = thriftPeers_.find(peerName);
  if (thriftPeerIt != thriftPeers_.end()) {
    return (
        getTimeSinceEpochMs() -
        *thriftPeerIt->second.peerSpec.stateElapsedTimeMs());
  }
  return 0;
}

/**
 * @brief static util function to fetch peer flaps
 *
 * @param peerName  name of the kvstore peer
 *
 * @return <int32_t>  number of flaps
 */
template <class ClientType>
int32_t
KvStoreDb<ClientType>::getCurrentFlaps(std::string const& peerName) {
  auto thriftPeerIt = thriftPeers_.find(peerName);
  if (thriftPeerIt != thriftPeers_.end()) {
    return *thriftPeerIt->second.peerSpec.flaps();
  }
  return 0;
}

// static util function for state transition
template <class ClientType>
thrift::KvStorePeerState
KvStoreDb<ClientType>::getNextState(
    std::optional<thrift::KvStorePeerState> const& currState,
    KvStorePeerEvent const& event) {
  //
  // This is the state transition matrix for KvStorePeerState. It is a
  // sparse-matrix with row representing `KvStorePeerState` and column
  // representing `KvStorePeerEvent`. State transition is driven by
  // certain event. Invalid state jump will cause fatal error.
  //
  static const std::vector<std::vector<std::optional<thrift::KvStorePeerState>>>
      stateMap = {/*
                   * index 0 - IDLE
                   * PEER_ADD => SYNCING
                   * THRIFT_API_ERROR => IDLE
                   * INCONSISTENCY_DETECTED => IDLE
                   */
                  {thrift::KvStorePeerState::SYNCING,
                   std::nullopt,
                   std::nullopt,
                   thrift::KvStorePeerState::IDLE,
                   thrift::KvStorePeerState::IDLE},
                  /*
                   * index 1 - SYNCING
                   * SYNC_RESP_RCVD => INITIALIZED
                   * THRIFT_API_ERROR => IDLE
                   * INCONSISTENCY_DETECTED => IDLE
                   */
                  {std::nullopt,
                   std::nullopt,
                   thrift::KvStorePeerState::INITIALIZED,
                   thrift::KvStorePeerState::IDLE,
                   thrift::KvStorePeerState::IDLE},
                  /*
                   * index 2 - INITIALIZED
                   * SYNC_RESP_RCVD => INITIALIZED
                   * THRIFT_API_ERROR => IDLE
                   * INCONSISTENCY_DETECTED => IDLE
                   */
                  {std::nullopt,
                   std::nullopt,
                   thrift::KvStorePeerState::INITIALIZED,
                   thrift::KvStorePeerState::IDLE,
                   thrift::KvStorePeerState::IDLE}};

  CHECK(currState.has_value()) << "Current state is 'UNDEFINED'";

  std::optional<thrift::KvStorePeerState> nextState =
      stateMap[static_cast<uint32_t>(currState.value())]
              [static_cast<uint32_t>(event)];

  CHECK(nextState.has_value()) << "Next state is 'UNDEFINED'";
  return nextState.value();
}

//
// KvStorePeer is the struct representing peer information including:
//  - thrift client;
//  - peerSpec;
//  - backoff;
//  - etc.
//
template <class ClientType>
KvStoreDb<ClientType>::KvStorePeer::KvStorePeer(
    const std::string& nodeName,
    const std::string& areaTag,
    const thrift::PeerSpec& ps,
    const ExponentialBackoff<std::chrono::milliseconds>& expBackoff,
    const KvStoreParams& kvParams)
    : nodeName(nodeName),
      areaTag(areaTag),
      peerSpec(ps),
      expBackoff(expBackoff),
      kvParams_(kvParams) {
  peerSpec.state() = thrift::KvStorePeerState::IDLE;
  CHECK(not this->nodeName.empty());
  CHECK(not this->areaTag.empty());
  CHECK(not this->peerSpec.peerAddr()->empty());
  CHECK(
      this->expBackoff.getInitialBackoff() <= this->expBackoff.getMaxBackoff());
  XLOG(INFO) << fmt::format(
      "node: {}, initial backoff {} and max backoff {}",
      nodeName,
      this->expBackoff.getInitialBackoff().count(),
      this->expBackoff.getMaxBackoff().count());
}

template <class ClientType>
folly::SemiFuture<folly::Unit>
KvStoreDb<ClientType>::KvStorePeer::setKvStoreKeyValsWrapper(
    const std::string& area, const thrift::KeySetParams& keySetParams) {
  if (not kvParams_.enable_secure_thrift_client) {
    return plainTextClient->semifuture_setKvStoreKeyVals(keySetParams, area);
  }
  // TLS fallback
  try {
    return secureClient->semifuture_setKvStoreKeyVals(keySetParams, area);
  } catch (const folly::AsyncSocketException& ex) {
    XLOG(ERR) << fmt::format("{} got exception: {}", __FUNCTION__, ex.what());
    fb303::fbData->addStatValue(
        "kvstore.thrift.semifuture_setKvStoreKeyVals.secure_client.failure",
        1,
        fb303::COUNT);
    return plainTextClient->semifuture_setKvStoreKeyVals(keySetParams, area);
  }
}

template <class ClientType>
folly::SemiFuture<thrift::Publication>
KvStoreDb<ClientType>::KvStorePeer::getKvStoreKeyValsFilteredAreaWrapper(
    const thrift::KeyDumpParams& filter, const std::string& area) {
  if (not kvParams_.enable_secure_thrift_client) {
    return plainTextClient->semifuture_getKvStoreKeyValsFilteredArea(
        filter, area);
  }
  // TLS fallback
  try {
    return secureClient->semifuture_getKvStoreKeyValsFilteredArea(filter, area);
  } catch (const folly::AsyncSocketException& ex) {
    XLOG(ERR) << fmt::format("{} got exception: {}", __FUNCTION__, ex.what());
    fb303::fbData->addStatValue(
        "kvstore.thrift.semifuture_getKvStoreKeyValsFilteredArea.secure_client.failure",
        1,
        fb303::COUNT);
    return plainTextClient->semifuture_getKvStoreKeyValsFilteredArea(
        filter, area);
  }
}

template <class ClientType>
bool
KvStoreDb<ClientType>::KvStorePeer::getOrCreateThriftClient(
    OpenrEventBase* evb, std::optional<int> maybeIpTos) {
  // use the existing thrift client if any
  if (kvParams_.enable_secure_thrift_client) {
    if (secureClient and plainTextClient) {
      return true;
    }
  } else {
    if (plainTextClient) {
      return true;
    }
  }

  try {
    XLOG(INFO) << fmt::format(
        "{} [Thrift Sync] Creating thrift client with addr: {}, port: {}, peerName: {}",
        areaTag,
        *peerSpec.peerAddr(),
        *peerSpec.ctrlPort(),
        nodeName);

    if (kvParams_.enable_secure_thrift_client) {
      auto context = std::make_shared<folly::SSLContext>();
      context->setVerificationOption(
          folly::SSLContext::VerifyServerCertificate::IF_PRESENTED);
      context->loadTrustedCertificates(kvParams_.x509_ca_path->c_str());
      // Thrift's Rocket transport requires an ALPN
      context->setAdvertisedNextProtocols({"rs"});
      context->loadCertificate(kvParams_.x509_cert_path->c_str());
      context->loadPrivateKey(kvParams_.x509_key_path->c_str());
      folly::ssl::SSLCommonOptions::setClientOptions(*context);
      // Since we are suggesting support for rocket in ALPN,
      // we should use RocketClientChannel to match what is negotiated
      secureClient = getOpenrCtrlSecureClient<ClientType>(
          *(evb->getEvb()),
          context,
          folly::IPAddress(*peerSpec.peerAddr()), /* v6LinkLocal */
          *peerSpec.ctrlPort(), /* port to establish TCP connection */
          Constants::kServiceConnSSLTimeout, /* client connection timeout */
          Constants::kServiceProcTimeout, /* request processing timeout */
          folly::AsyncSocket::anyAddress(), /* bindAddress */
          maybeIpTos, /* IP_TOS value for control plane */
          true /* enable socket keepalive */);
      fb303::fbData->addStatValue(
          "kvstore.thrift.secure_client", 1, fb303::COUNT);
    }
    plainTextClient = getOpenrCtrlPlainTextClient<ClientType>(
        *(evb->getEvb()),
        folly::IPAddress(*peerSpec.peerAddr()), /* v6LinkLocal */
        *peerSpec.ctrlPort(), /* port to establish TCP connection */
        Constants::kServiceConnTimeout, /* client connection timeout */
        Constants::kServiceProcTimeout, /* request processing timeout */
        folly::AsyncSocket::anyAddress(), /* bindAddress */
        maybeIpTos /* IP_TOS value for control plane */,
        true /* enable socket keepalive */);

    fb303::fbData->addStatValue(
        "kvstore.thrift.plaintext_client", 1, fb303::COUNT);
  } catch (std::exception const& e) {
    XLOG(ERR) << fmt::format(
        "{} [Thrift Sync] Failed creating thrift client with addr: {}, port: {}, peerName: {}. Exception: {}",
        areaTag,
        *peerSpec.peerAddr(),
        *peerSpec.ctrlPort(),
        nodeName,
        folly::exceptionStr(e));

    // record telemetry for thrift calls
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_client_connection_failure", 1, fb303::COUNT);

    // clean up state for next round of scanning
    plainTextClient.reset();
    secureClient.reset();
    expBackoff.reportError(); // apply exponential backoff
    return false;
  }
  return true;
}

/*
 * KvStoreDb is the class instance that maintains the KV pairs with internal
 * map per AREA. KvStoreDb will sync with peers to maintain eventual
 * consistency. It supports external message exchanging through Thrift channel.
 *
 * NOTE Monitoring:
 * This module exposes fb303 counters that can be leveraged for monitoring
 * KvStoreDb's correctness and performance behevior in production
 *
 * kvstore.thrift.secure_client: # of secure client creation;
 * kvstore.thrift.plaintext_client: # of plaintext client creation;
 * kvstore.thrift.num_client_connection_failure: # of client creation failures;
 * kvstore.thrift.num_full_sync: # of full-sync performed;
 * kvstore.thrift.num_missing_keys: # of missing keys from syncing with peer;
 * kvstore.thrift.num_full_sync_success: # of successful full-sync performed;
 * kvstore.thrift.num_full_sync_failure: # of failed full-sync performed;
 * kvstore.thrift.full_sync_duration_ms: avg time elapsed for a full-sync req;
 *
 * kvstore.thrift.num_flood_pub: # of flooding req issued;
 * kvstore.thrift.num_flood_key_vals: # of keyVals one flooding req contains;
 * kvstore.thrift.num_flood_pub_success: # of successful flooding req performed;
 * kvstore.thrift.num_flood_pub_failure: # of failed flooding req performed;
 * kvstore.thrift.flood_pub_duration_ms: avg time elapsed for a flooding req;
 *
 * kvstore.thrift.num_finalized_sync: # of finalized finalized-sync performed;
 * kvstore.thrift.num_finalized_sync_success: # of successful finalized-sync;
 * kvstore.thrift.num_finalized_sync_failure: # of failed finalized-sync;
 * kvstore.thrift.finalized_sync_duration_ms: avg time elapsed for a req;
 */
template <class ClientType>
KvStoreDb<ClientType>::KvStoreDb(
    OpenrEventBase* evb,
    KvStoreParams& kvParams,
    const std::string& area,
    const std::string& nodeId,
    std::function<void()> initialKvStoreSyncedCallback)
    : kvParams_(kvParams),
      area_(area),
      areaTag_(fmt::format("[Area {}] ", area)),
      initialKvStoreSyncedCallback_(initialKvStoreSyncedCallback),
      evb_(evb) {
  if (kvParams_.floodRate) {
    floodLimiter_ = std::make_unique<folly::BasicTokenBucket<>>(
        *kvParams_.floodRate->flood_msg_per_sec(),
        *kvParams_.floodRate->flood_msg_burst_size());
    pendingPublicationTimer_ =
        folly::AsyncTimeout::make(*evb_->getEvb(), [this]() noexcept {
          if (!floodLimiter_->consume(1)) {
            pendingPublicationTimer_->scheduleTimeout(
                Constants::kFloodPendingPublication);
            return;
          }
          floodBufferedUpdates();
        });
  }

  XLOG(INFO) << fmt::format(
      "{} Starting kvstore DB instance for node: {}", AreaTag(), nodeId);

  {
    // Create a fiber task to periodically dump flooding topology.
    auto fiber = evb_->addFiberTaskFuture([this]() mutable noexcept {
      XLOG(DBG1) << AreaTag() << "Starting flood-topo dump task";
      floodTopoDumpTask();
      XLOG(DBG1)
          << fmt::format("[Exit] {} Flood-topo dump task finished.", AreaTag());
    });
    kvStoreDbWorkers_.emplace_back(std::move(fiber));
  }

  {
    // Create a fiber task to periodically check adj key ttl.
    auto fiber = evb_->addFiberTaskFuture([this]() mutable noexcept {
      XLOG(DBG1) << AreaTag() << "Starting ttl-check task";
      checkKeyTtlTask();
      XLOG(DBG1)
          << fmt::format("[Exit] {} Ttl-check task finished.", AreaTag());
    });
    kvStoreDbWorkers_.emplace_back(std::move(fiber));
  }

  // Perform full-sync if there are peers to sync with.
  thriftSyncTimer_ = folly::AsyncTimeout::make(
      *evb_->getEvb(), [this]() noexcept { requestThriftPeerSync(); });

  // Hook up timer with cleanupTtlCountdownQueue(). The actual scheduling
  // happens within updateTtlCountdownQueue()
  ttlCountdownTimer_ = folly::AsyncTimeout::make(
      *evb_->getEvb(), [this]() noexcept { cleanupTtlCountdownQueue(); });

  // Create ttl timer for refreshing ttls of self-originated key-vals
  selfOriginatedKeyTtlTimer_ = folly::AsyncTimeout::make(
      *evb_->getEvb(), [this]() noexcept { advertiseTtlUpdates(); });

  // Create timer to advertise pending key-vals
  advertiseKeyValsTimer_ =
      folly::AsyncTimeout::make(*evb_->getEvb(), [this]() noexcept {
        // Advertise all pending keys
        advertiseSelfOriginatedKeys();

        // Clear all backoff if they are passed away
        for (auto& [key, selfOriginatedVal] : selfOriginatedKeyVals_) {
          auto& backoff = selfOriginatedVal.keyBackoff;
          if (backoff.has_value() and backoff.value().canTryNow()) {
            XLOG(DBG2) << "Clearing off the exponential backoff for key "
                       << key;
            backoff.value().reportSuccess();
          }
        }
      });

  // create throttled fashion of ttl update
  selfOriginatedTtlUpdatesThrottled_ = std::make_unique<AsyncThrottle>(
      evb_->getEvb(),
      Constants::kKvStoreSyncThrottleTimeout,
      [this]() noexcept { advertiseTtlUpdates(); });

  // create throttled fashion of advertising pending keys
  advertiseSelfOriginatedKeysThrottled_ = std::make_unique<AsyncThrottle>(
      evb_->getEvb(),
      Constants::kKvStoreSyncThrottleTimeout,
      [this]() noexcept { advertiseSelfOriginatedKeys(); });

  // create throttled fashion of unsetting pending keys
  unsetSelfOriginatedKeysThrottled_ = std::make_unique<AsyncThrottle>(
      evb_->getEvb(),
      Constants::kKvStoreClearThrottleTimeout,
      [this]() noexcept { unsetPendingSelfOriginatedKeys(); });

  // initialize KvStore per-area counters
  fb303::fbData->addStatExportType(
      "kvstore.updated_key_vals." + area, fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.received_key_vals." + area, fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.received_publications." + area, fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.num_expiring_keys." + area, fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.num_flood_peers." + area, fb303::SUM);
}

template <class ClientType>
void
KvStoreDb<ClientType>::stop() {
  XLOG(DBG1)
      << fmt::format("[Exit] Mark kvStoreDb with area {} stopped", AreaTag());

  // Mark this kvStoreDb stopped to no longer processing data
  isStopped_ = true;

  // Send stop signal for internal fibers
  floodTopoStopSignal_.post();
  ttlCheckStopSignal_.post();

  // waits for all child fibers to complete
  folly::collectAll(kvStoreDbWorkers_.begin(), kvStoreDbWorkers_.end()).get();

  // NOTE: folly::AsyncTimeout and AsyncThrottle must be tear-down in evb loop
  evb_->getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    // Reverse order to reset throttle
    unsetSelfOriginatedKeysThrottled_.reset();
    advertiseSelfOriginatedKeysThrottled_.reset();
    selfOriginatedTtlUpdatesThrottled_.reset();

    // Reverse order to reset timer
    advertiseKeyValsTimer_.reset();
    selfOriginatedKeyTtlTimer_.reset();
    ttlCountdownTimer_.reset();
    thriftSyncTimer_.reset();

    if (kvParams_.floodRate) {
      pendingPublicationTimer_.reset();
    }

    // Clean up peer with client connection
    thriftPeers_.clear();
  });

  XLOG(INFO)
      << fmt::format("[Exit] {} Successfully stopped KvStoreDb.", AreaTag());
}

template <class ClientType>
void
KvStoreDb<ClientType>::floodTopoDumpTask() noexcept {
  while (true) { // Break when stop signal is ready
    // Sleep before next check
    // ATTN: sleep first to avoid empty peers when KvStoreDb initially starts.
    if (floodTopoStopSignal_.try_wait_for(Constants::kFloodTopoDumpInterval)) {
      break; // Baton was posted
    } else {
      floodTopoStopSignal_.reset(); // Baton experienced timeout
    }
    floodTopoDump();
  } // while
}

template <class ClientType>
void
KvStoreDb<ClientType>::floodTopoDump() noexcept {
  const auto& floodPeers = getFloodPeers();

  XLOG(INFO) << AreaTag()
             << fmt::format(
                    "[Flood Topo] NodeId: {}, flooding peers: [{}]",
                    kvParams_.nodeId,
                    folly::join(",", floodPeers));

  // Expose number of flood peers into ODS counter
  fb303::fbData->addStatValue(
      "kvstore.num_flood_peers." + area_, floodPeers.size(), fb303::SUM);
}

template <class ClientType>
void
KvStoreDb<ClientType>::checkKeyTtlTask() noexcept {
  while (true) { // Break when stop signal is ready
    // Sleep before next check
    // ATTN: sleep first to avoid empty peers when KvStoreDb initially starts.
    if (ttlCheckStopSignal_.try_wait_for(Constants::kFloodTopoDumpInterval)) {
      break; // Baton was posted
    } else {
      ttlCheckStopSignal_.reset(); // Baton experienced timeout
    }
    checkKeyTtl();
  } // while
}

template <class ClientType>
void
KvStoreDb<ClientType>::checkKeyTtl() noexcept {
  // total number of unexpected keys below ttl alert threshold
  auto cnt{0};

  KvStoreFilters filter{
      {Constants::kAdjDbMarker.toString()}, /* match "adj:" key only */
      {}, /* originator match */
      thrift::FilterOperator::OR /* matching type */};

  for (auto const& [k, v] : kvStore_) {
    if (not filter.keyMatch(k, v)) {
      continue;
    }
    // ATTN: ttl is refreshed every keyTtl.count() / 4 by default.
    // Increment the counter if the following condition fulfilled:
    //
    // 1. If keyTtl.count() is below the threshold of 1/2 keyTtl, this
    // indicates that the ttl-refreshing sent from peer on timstamp of
    // {3/4, 1/2} keyTtl are NOT received;
    //
    // 2. If the originator of this adj key is still connected to KvStore,
    // this is a strong signal that flooding topo is in bad state;
    if (*v.ttl() < kvParams_.keyTtl.count() / 2 and
        thriftPeers_.count(*v.originatorId())) {
      XLOG(ERR) << fmt::format(
          "Ttl of {} drops below 50% threshold and going to expire", k);
      cnt += 1;
    }
  }

  // Expose number of about-to-expire adj keys into ODS counter
  fb303::fbData->addStatValue(
      "kvstore.num_expiring_keys." + area_, cnt, fb303::SUM);
}

template <class ClientType>
void
KvStoreDb<ClientType>::setSelfOriginatedKey(
    std::string const& key, std::string const& value, uint32_t version) {
  XLOG(DBG3) << AreaTag()
             << fmt::format("{} called for key: {}", __FUNCTION__, key);

  // Create 'thrift::Value' object which will be sent to KvStore
  thrift::Value thriftValue = createThriftValue(
      version,
      kvParams_.nodeId,
      value,
      kvParams_.keyTtl.count(),
      0 /* ttl version */,
      0 /* hash */);
  CHECK(thriftValue.value());

  // Use one version number higher than currently in KvStore if not specified
  if (not version) {
    auto it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      thriftValue.version() = *it->second.version() + 1;
    } else {
      thriftValue.version() = 1;
    }
  }

  // Store self-originated key-vals in cache
  // ATTN: ttl backoff will be set separately in scheduleTtlUpdates()
  auto selfOriginatedVal = SelfOriginatedValue(thriftValue);
  selfOriginatedKeyVals_[key] = std::move(selfOriginatedVal);

  // Advertise key to KvStore
  thrift::KeyVals keyVals = {{key, thriftValue}};
  thrift::KeySetParams params;
  params.keyVals() = std::move(keyVals);
  setKeyVals(std::move(params), true /* self-originated update */);

  // Add ttl backoff and trigger selfOriginatedKeyTtlTimer_
  scheduleTtlUpdates(key, false /* advertiseImmediately */);
}

template <class ClientType>
void
KvStoreDb<ClientType>::persistSelfOriginatedKey(
    std::string const& key, std::string const& value) {
  XLOG(DBG3) << AreaTag()
             << fmt::format("{} called for key: {}", __FUNCTION__, key);

  // Look key up in local cached storage
  auto selfOriginatedKeyIt = selfOriginatedKeyVals_.find(key);

  // Advertise key-val if old key-val needs to be overridden or key does not
  // exist in KvStore already.
  bool shouldAdvertise = false;

  // Create the default thrift value with:
  //  1. version - [NOT FILLED] - 0 is INVALID
  //  2. originatorId - [DONE] - nodeId
  //  3. value - [DONE] - value
  //  4. ttl - [DONE] - kvParams_.keyTtl.count()
  //  5. ttlVersion - [NOT FILLED] - empty
  //  6. hash - [OPTIONAL] - empty
  thrift::Value thriftValue =
      createThriftValue(0, kvParams_.nodeId, value, kvParams_.keyTtl.count());
  CHECK(thriftValue.value());

  // Two cases for this particular (k, v) pair:
  //  1) Key is first-time persisted:
  //     Retrieve it from `kvStore_`.
  //      <1> Key is NOT found in `KvStore` (ATTN:
  //          new key advertisement)
  //      <2> Key is found in `KvStore`. Override the
  //          value with authoritative operation.
  //  2) Key has been persisted before:
  //     Retrieve it from cached self-originated key-vals;
  if (selfOriginatedKeyIt == selfOriginatedKeyVals_.end()) {
    // Key is first-time persisted. Check if key is in KvStore.
    auto keyIt = kvStore_.find(key);
    if (keyIt == kvStore_.end()) {
      // Key is not in KvStore. Set initial version and ready to advertise.
      thriftValue.version() = 1;
      shouldAdvertise = true;
    } else {
      // Key is NOT persisted but can be found inside KvStore.
      // This can be keys advertised by our previous incarnation.
      thriftValue = keyIt->second;
      // TTL update pub is never saved in kvstore. Value is not std::nullopt.
      DCHECK(thriftValue.value());
    }
  } else {
    // Key has been persisted before
    thriftValue = selfOriginatedKeyIt->second.value;
    if (*thriftValue.value() == value) {
      // this is a no op, return early and change no state
      return;
    }
  }

  // Override thrift::Value if
  //  1) the SAME key is originated by different node;
  //  2) the peristed value has changed;
  if (*thriftValue.originatorId() != kvParams_.nodeId or
      *thriftValue.value() != value) {
    (*thriftValue.version())++;
    thriftValue.ttlVersion() = 0;
    thriftValue.value() = value;
    thriftValue.originatorId() = kvParams_.nodeId;
    shouldAdvertise = true;
  }

  // Override ttl value to new one.
  // ATTN: When ttl changes but value doesn't, we should advertise ttl
  // immediately so that new ttl is in effect.
  const bool hasTtlChanged =
      (kvParams_.keyTtl.count() != *thriftValue.ttl()) ? true : false;
  thriftValue.ttl() = kvParams_.keyTtl.count();

  // Cache it in selfOriginatedKeyVals. Override the existing one.
  if (selfOriginatedKeyIt == selfOriginatedKeyVals_.end()) {
    auto selfOriginatedVal = SelfOriginatedValue(std::move(thriftValue));
    selfOriginatedKeyVals_.emplace(key, std::move(selfOriginatedVal));
  } else {
    selfOriginatedKeyIt->second.value = std::move(thriftValue);
  }

  // Override existing backoff as well
  selfOriginatedKeyVals_[key].keyBackoff =
      ExponentialBackoff<std::chrono::milliseconds>(
          Constants::kInitialBackoff, Constants::kMaxBackoff);

  // Add keys to list of pending keys
  if (shouldAdvertise) {
    keysToAdvertise_.insert(key);
  }

  // Throttled advertisement of pending keys
  advertiseSelfOriginatedKeysThrottled_->operator()();

  // Add ttl backoff and trigger selfOriginatedKeyTtlTimer_
  scheduleTtlUpdates(key, hasTtlChanged /* advertiseImmediately */);
}

template <class ClientType>
void
KvStoreDb<ClientType>::advertiseSelfOriginatedKeys() {
  XLOG(DBG3) << "Advertising Self Originated Keys. Num keys to advertise: "
             << keysToAdvertise_.size();

  // advertise pending key for each area
  if (keysToAdvertise_.empty()) {
    return;
  }

  // Build set of keys to advertise
  thrift::KeyVals keyVals{};
  // Build keys to be cleaned from local storage
  std::vector<std::string> keysToClear;

  std::chrono::milliseconds timeout = kvParams_.syncMaxBackoff;
  for (auto const& key : keysToAdvertise_) {
    // Each key was introduced through a persistSelfOriginatedKey() call.
    // Therefore, each key is in selfOriginatedKeyVals_ and has a keyBackoff.
    auto& selfOriginatedValue = selfOriginatedKeyVals_.at(key);
    const auto& thriftValue = selfOriginatedValue.value;
    CHECK(selfOriginatedValue.keyBackoff.has_value());

    auto& backoff = selfOriginatedValue.keyBackoff.value();

    // Proceed only if key backoff is active
    if (not backoff.canTryNow()) {
      XLOG(DBG2) << AreaTag() << fmt::format("Skipping key: {}", key);
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
      continue;
    }

    // Apply backoff
    backoff.reportError();
    timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

    printKeyValInArea(
        1 /*logLevel*/, "Advertising key update", AreaTag(), key, thriftValue);
    // Set in keyVals which is going to be advertise to the kvStore.
    DCHECK(thriftValue.value());
    keyVals.emplace(key, thriftValue);
    keysToClear.emplace_back(key);
  }

  // Advertise key-vals to KvStore
  thrift::KeySetParams params;
  params.keyVals() = std::move(keyVals);
  setKeyVals(std::move(params), true /* self-originated update */);

  // clear out variable used for batching advertisements
  for (auto const& key : keysToClear) {
    keysToAdvertise_.erase(key);
  }

  // Schedule next-timeout for processing/clearing backoffs
  XLOG(DBG2) << "Scheduling timer after " << timeout.count() << "ms.";
  advertiseKeyValsTimer_->scheduleTimeout(timeout);
}

template <class ClientType>
void
KvStoreDb<ClientType>::unsetSelfOriginatedKey(
    std::string const& key, std::string const& value) {
  XLOG(DBG3) << AreaTag()
             << fmt::format("{} called for key: {}", __FUNCTION__, key);

  // erase key
  eraseSelfOriginatedKey(key);

  // Check if key is in KvStore. If key doesn't exist in KvStore no need to add
  // it as "empty". This condition should not exist.
  auto keyIt = kvStore_.find(key);
  if (keyIt == kvStore_.end()) {
    return;
  }

  // Overwrite all values and increment version.
  auto thriftValue = keyIt->second;
  thriftValue.originatorId() = kvParams_.nodeId;
  (*thriftValue.version())++;
  thriftValue.ttlVersion() = 0;
  thriftValue.value() = value;

  keysToUnset_.emplace(key, std::move(thriftValue));
  // Send updates to KvStore via batch processing.
  unsetSelfOriginatedKeysThrottled_->operator()();
}

template <class ClientType>
void
KvStoreDb<ClientType>::eraseSelfOriginatedKey(std::string const& key) {
  XLOG(DBG3) << AreaTag()
             << fmt::format("{} called for key: {}", __FUNCTION__, key);
  selfOriginatedKeyVals_.erase(key);
  keysToAdvertise_.erase(key);
}

template <class ClientType>
void
KvStoreDb<ClientType>::unsetPendingSelfOriginatedKeys() {
  if (keysToUnset_.empty()) {
    return;
  }

  // Build set of keys to update KvStore
  thrift::KeyVals keyVals;
  // Build keys to be cleaned from local storage. Do not remove from
  // keysToUnset_ directly while iterating.
  std::vector<std::string> localKeysToUnset;

  for (auto const& [key, thriftVal] : keysToUnset_) {
    // ATTN: consider corner case of key X being:
    //  Case 1) first persisted then unset before throttling triggers
    //    X will NOT be persisted at all.
    //
    //  Case 2) first unset then persisted before throttling kicks in
    //    X will NOT be unset since it is inside `persistedKeyVals_`
    //
    //  Source of truth will be `persistedKeyVals_` as
    //  `unsetSelfOriginatedKey()` will do `eraseSelfOriginatedKey()`, which
    //  wipes out its existence.
    auto it = selfOriginatedKeyVals_.find(key);
    if (it == selfOriginatedKeyVals_.end()) {
      // Case 1:  X is not persisted. Set new value.
      printKeyValInArea(1 /*logLevel*/, "Unsetting", AreaTag(), key, thriftVal);
      keyVals.emplace(key, thriftVal);
      localKeysToUnset.emplace_back(key);
    } else {
      // Case 2: X is persisted. Do not set new value.
      localKeysToUnset.emplace_back(key);
    }
  }

  // Send updates to KvStore
  thrift::KeySetParams params;
  params.keyVals() = std::move(keyVals);
  setKeyVals(std::move(params), true /* self-originated update */);

  // Empty out keysToUnset_
  for (auto const& key : localKeysToUnset) {
    keysToUnset_.erase(key);
  }
}

template <class ClientType>
void
KvStoreDb<ClientType>::scheduleTtlUpdates(
    std::string const& key, bool advertiseImmediately) {
  int64_t ttl = kvParams_.keyTtl.count();

  auto& value = selfOriginatedKeyVals_.at(key);

  // renew before ttl expires. renew every ttl/4, i.e., try 3 times using
  // ExponetialBackoff to track remaining time before ttl expiration.
  value.ttlBackoff = ExponentialBackoff<std::chrono::milliseconds>(
      std::chrono::milliseconds(ttl / 4),
      std::chrono::milliseconds(ttl / 4 + 1));

  // Delay first ttl advertisement by (ttl / 4). We have just advertised key or
  // update and would like to avoid sending unncessary immediate ttl update
  if (not advertiseImmediately) {
    selfOriginatedKeyVals_[key].ttlBackoff.reportError();
  }

  // Trigger timer to advertise ttl updates for self-originated key-vals.
  selfOriginatedTtlUpdatesThrottled_->operator()();
}

template <class ClientType>
void
KvStoreDb<ClientType>::advertiseTtlUpdates() {
  // Build set of keys to advertise ttl updates
  auto timeout = Constants::kMaxTtlUpdateInterval;

  // all key-vals to advertise ttl updates for
  thrift::KeyVals keyVals;

  for (auto& [key, val] : selfOriginatedKeyVals_) {
    auto& thriftValue = val.value;
    auto& backoff = val.ttlBackoff;
    if (not backoff.canTryNow()) {
      XLOG(DBG2) << AreaTag() << fmt::format("Skipping key: {}", key);

      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
      continue;
    }

    // Apply backoff
    backoff.reportError();
    timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

    // Bump ttl version
    (*thriftValue.ttlVersion())++;

    // Create copy of thrift::Value without value field for bandwidth efficiency
    // when advertising
    auto advertiseValue = createThriftValue(
        *thriftValue.version(),
        kvParams_.nodeId,
        std::nullopt, /* empty value */
        *thriftValue.ttl(), /* ttl */
        *thriftValue.ttlVersion()) /* ttl version */;

    // Set in keyVals which will be advertised to the kvStore
    DCHECK(not advertiseValue.value());
    printKeyValInArea(
        1 /* logLevel */,
        "Advertising ttl update",
        AreaTag(),
        key,
        advertiseValue);
    keyVals.emplace(key, advertiseValue);
  }

  // Advertise to KvStore
  if (not keyVals.empty()) {
    thrift::KeySetParams params;
    params.keyVals() = std::move(keyVals);
    setKeyVals(std::move(params), true /* self-originated update */);
  }

  // Schedule next-timeout for processing/clearing backoffs
  XLOG(DBG2)
      << AreaTag()
      << fmt::format("Scheduling ttl timer after {}ms.", timeout.count());

  selfOriginatedKeyTtlTimer_->scheduleTimeout(timeout);
}

template <class ClientType>
thrift::SetKeyValsResult
KvStoreDb<ClientType>::setKeyVals(
    thrift::KeySetParams&& setParams, bool isSelfOriginatedUpdate) {
  // Update statistics
  fb303::fbData->addStatValue("kvstore.cmd_key_set", 1, fb303::COUNT);

  // Update hash for key-values
  for (auto& [_, value] : *setParams.keyVals()) {
    if (value.value().has_value()) {
      value.hash() =
          generateHash(*value.version(), *value.originatorId(), value.value());
    }
  }

  // Create publication and merge it with local KvStore
  thrift::Publication rcvdPublication;
  rcvdPublication.keyVals() = std::move(*setParams.keyVals());
  rcvdPublication.nodeIds().move_from(setParams.nodeIds());
  auto pub = mergePublication(rcvdPublication, isSelfOriginatedUpdate);
  thrift::SetKeyValsResult result;
  result.noMergeReasons() = std::move(*pub.noMergeKeyVals());
  return result;
}

template <class ClientType>
void
KvStoreDb<ClientType>::updateTtlCountdownQueue(
    const thrift::Publication& publication, bool isSelfOriginatedUpdate) {
  for (const auto& [key, value] : *publication.keyVals()) {
    // self originated key should never expire
    // Explicit deletion use separate logic
    if (not isSelfOriginatedUpdate and selfOriginatedKeyVals_.count(key) > 0) {
      continue;
    }

    if (*value.ttl() != Constants::kTtlInfinity) {
      TtlCountdownQueueEntry queueEntry;
      queueEntry.expiryTime = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(*value.ttl());
      queueEntry.key = key;
      queueEntry.version = *value.version();
      queueEntry.ttlVersion = *value.ttlVersion();
      queueEntry.originatorId = *value.originatorId();

      if ((ttlCountdownQueue_.empty() or
           (queueEntry.expiryTime <= ttlCountdownQueue_.top().expiryTime)) and
          ttlCountdownTimer_) {
        // Reschedule the shorter timeout
        ttlCountdownTimer_->scheduleTimeout(
            std::chrono::milliseconds(*value.ttl()));
      }

      ttlCountdownQueue_.push(std::move(queueEntry));
    }
  }
}

// loop through all key/vals and count the size of KvStoreDB (per area)
template <class ClientType>
size_t
KvStoreDb<ClientType>::getKeyValsSize() const {
  size_t size = 0;
  if (kvStore_.empty()) {
    return size;
  }
  // calculate total of struct members with fixed size once at the beginning
  size_t fixed_size =
      kvStore_.size() * (sizeof(std::string) + sizeof(thrift::Value));

  // loop through all key/vals and add size of each KV entry
  for (auto const& [key, value] : kvStore_) {
    size += key.size() + value.originatorId()->size() + value.value()->size();
  }
  size += fixed_size;

  return size;
}

// build publication out of the requested keys (per request)
// if not keys provided, will return publication with empty keyVals
template <class ClientType>
thrift::Publication
KvStoreDb<ClientType>::getKeyVals(std::vector<std::string> const& keys) {
  thrift::Publication thriftPub;
  thriftPub.area() = area_;

  for (auto const& key : keys) {
    // if requested key if found, respond with version and value
    auto it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      // copy here
      thriftPub.keyVals()[key] = it->second;
    }
  }
  return thriftPub;
}

// This function serves the purpose of periodically scanning peers in
// IDLE state and promote them to SYNCING state. The initial dump will
// happen in async nature to unblock KvStore to process other requests.
template <class ClientType>
void
KvStoreDb<ClientType>::requestThriftPeerSync() {
  // minimal timeout for next run
  auto timeout = kvParams_.syncMaxBackoff;

  // pre-fetch of peers in "SYNCING" state for later calculation
  uint32_t numThriftPeersInSync =
      getPeersByState(thrift::KvStorePeerState::SYNCING).size();

  // Scan over thriftPeers to promote IDLE peers to SYNCING
  for (auto& [peerName, thriftPeer] : thriftPeers_) {
    auto& peerSpec = thriftPeer.peerSpec; // thrift::PeerSpec
    const auto& expBackoff = thriftPeer.expBackoff;

    // ignore peers in state other than IDLE
    if (*peerSpec.state() != thrift::KvStorePeerState::IDLE) {
      continue;
    }

    // update the global minimum timeout value for next try
    if (not expBackoff.canTryNow()) {
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      continue;
    }

    // create thrift client and do backoff if can't go through
    if (not thriftPeer.getOrCreateThriftClient(evb_, kvParams_.maybeIpTos)) {
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      continue;
    }

    // state transition
    auto oldState = *peerSpec.state();
    peerSpec.state() = getNextState(oldState, KvStorePeerEvent::PEER_ADD);
    peerSpec.stateEpochTimeMs() = getTimeSinceEpochMs();
    logStateTransition(peerName, oldState, *peerSpec.state());

    // mark peer from IDLE -> SYNCING
    numThriftPeersInSync += 1;

    // build KeyDumpParam
    thrift::KeyDumpParams params;
    KvStoreFilters kvFilters(
        std::vector<std::string>{}, /* keyPrefix list */
        std::set<std::string>{} /* originatorId list */);
    // ATTN: dump hashes instead of full key-val pairs with values
    auto thriftPub = dumpHashWithFilters(area_, kvStore_, kvFilters);
    params.keyValHashes() = *thriftPub.keyVals();
    params.senderId() = kvParams_.nodeId;

    // record telemetry for initial full-sync
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_full_sync", 1, fb303::COUNT);

    XLOG(INFO) << AreaTag()
               << fmt::format(
                      "[Thrift Sync] Initiating full-sync request for peer: {}",
                      peerName);

    // send request over thrift client and attach callback
    auto startTime = std::chrono::steady_clock::now();
    auto sf = thriftPeer.getKvStoreKeyValsFilteredAreaWrapper(params, area_);
    std::move(sf)
        .via(evb_->getEvb())
        .thenValue(
            [this, peer = peerName, startTime](thrift::Publication&& pub) {
              // state transition to INITIALIZED
              auto endTime = std::chrono::steady_clock::now();
              auto timeDelta =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      endTime - startTime);
              processThriftSuccess(peer, std::move(pub), timeDelta);
            })
        .thenError([this, peer = peerName, startTime](
                       const folly::exception_wrapper& ew) {
          // state transition to IDLE
          auto endTime = std::chrono::steady_clock::now();
          auto timeDelta =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  endTime - startTime);
          processThriftFailure(
              peer,
              fmt::format("FULL_SYNC failure with {}, {}", peer, ew.what()),
              timeDelta);

          // record telemetry for thrift calls
          fb303::fbData->addStatValue(
              "kvstore.thrift.num_full_sync_failure", 1, fb303::COUNT);
        });

    // in case pending peer size is over parallelSyncLimit,
    // wait until syncInitialBackoff before sending next round of sync
    if (numThriftPeersInSync > parallelSyncLimitOverThrift_) {
      timeout = kvParams_.syncInitialBackoff;
      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[Thrift Sync] {} peers are syncing in progress. Over limit: {}",
                 numThriftPeersInSync,
                 parallelSyncLimitOverThrift_);
      break;
    }
  } // for loop

  // process the rest after min timeout if NOT scheduled
  uint32_t numThriftPeersInIdle =
      getPeersByState(thrift::KvStorePeerState::IDLE).size();
  if (numThriftPeersInIdle > 0 or
      numThriftPeersInSync > parallelSyncLimitOverThrift_) {
    XLOG_IF(INFO, numThriftPeersInIdle)
        << AreaTag()
        << fmt::format(
               "[Thrift Sync] {} idle peers require full-sync. Schedule after: {}ms",
               numThriftPeersInIdle,
               timeout.count());
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(timeout));
  }
}

// This function will process the full-dump response from peers:
//  1) Merge peer's publication with local KvStoreDb;
//  2) Send a finalized full-sync to peer for missing keys;
//  3) Exponetially update number of peers to SYNC in parallel;
//  4) Promote KvStorePeerState from SYNCING -> INITIALIZED;
template <class ClientType>
void
KvStoreDb<ClientType>::processThriftSuccess(
    std::string const& peerName,
    thrift::Publication&& pub,
    std::chrono::milliseconds timeDelta) {
  // check if this kvStore is destructed and stop processing callbacks
  if (isStopped_) {
    return;
  }

  // check if it is valid peer(i.e. peer removed in process of syncing)
  if (not thriftPeers_.count(peerName)) {
    XLOG(WARNING)
        << AreaTag()
        << fmt::format(
               "[Thrift Sync] Invalid peer: {}. Skip state transition.",
               peerName);
    return;
  }

  // ATTN: In parallel link case, peer state can be set to IDLE when
  //       parallel adj comes up before the previous full-sync response
  //       being received. KvStoreDb will ignore the old full-sync
  //       response and will rely on the new full-sync response to
  //       promote the state.
  auto& peer = thriftPeers_.at(peerName);
  if (*peer.peerSpec.state() == thrift::KvStorePeerState::IDLE) {
    XLOG(WARNING)
        << AreaTag()
        << fmt::format(
               "[Thrift Sync] Ignore response from: {} due to IDLE state.",
               peerName);
    return;
  }

  auto numMissingKeys =
      pub.tobeUpdatedKeys().has_value() ? pub.tobeUpdatedKeys()->size() : 0;
  auto numReceivedKeys = pub.keyVals()->size();

  // ATTN: `peerName` is MANDATORY to fulfill the finialized
  //       full-sync with peers.
  const auto mergeResult = mergePublication(
      pub, false /* remote update */, peerName /* request finalized sync */);

  const auto kvUpdateCnt = mergeResult.keyVals()->size();

  // record telemetry for thrift calls
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_full_sync_success", 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.thrift.full_sync_duration_ms", timeDelta.count(), fb303::AVG);
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_missing_keys", numMissingKeys, fb303::SUM);
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_keyvals_update", kvUpdateCnt, fb303::SUM);

  XLOG(INFO)
      << AreaTag()
      << fmt::format(
             "[Thrift Sync] Full-sync response received from: {}", peerName)
      << fmt::format(
             " with {} key-vals and {} missing keys. ",
             numReceivedKeys,
             numMissingKeys)
      << fmt::format(
             "Incurred {} key-value updates. Processing time: {}ms",
             kvUpdateCnt,
             timeDelta.count());

  // State transition
  auto oldState = *peer.peerSpec.state();
  peer.peerSpec.state() =
      getNextState(oldState, KvStorePeerEvent::SYNC_RESP_RCVD);

  if (oldState != *peer.peerSpec.state()) {
    peer.peerSpec.stateEpochTimeMs() = getTimeSinceEpochMs();
    if (*peer.peerSpec.state() == thrift::KvStorePeerState::INITIALIZED) {
      peer.peerSpec.flaps() = *peer.peerSpec.flaps() + 1;
    }
  }
  logStateTransition(peerName, oldState, *peer.peerSpec.state());

  // Log full-sync event via replicate queue
  logSyncEvent(peerName, timeDelta);

  // Successfully received full-sync response. Double the parallel
  // sync limit. This is to:
  //  1) accelerate the rest of pending full-syncs if any;
  //  2) assume subsequeny sync diff will be small in traffic amount;
  parallelSyncLimitOverThrift_ = std::min(
      2 * parallelSyncLimitOverThrift_,
      Constants::kMaxFullSyncPendingCountThreshold);

  // Schedule another round of `thriftSyncTimer_` full-sync request if
  // there is still peer in IDLE state. If no IDLE peer, cancel timeout.
  uint32_t numThriftPeersInIdle =
      getPeersByState(thrift::KvStorePeerState::IDLE).size();
  if (numThriftPeersInIdle > 0) {
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  } else {
    thriftSyncTimer_->cancelTimeout();
  }

  // Fully synced with peers, check whether initial sync is completed.
  if (not initialSyncCompleted_) {
    processInitializationEvent();
  }
}

template <class ClientType>
void
KvStoreDb<ClientType>::processInitializationEvent() {
  int initialSyncSuccessCnt = 0;
  int initialSyncFailureCnt = 0;
  for (const auto& [peerName, peerStore] : thriftPeers_) {
    if (*peerStore.peerSpec.state() == thrift::KvStorePeerState::INITIALIZED) {
      // Achieved INITIALIZED state.
      ++initialSyncSuccessCnt;
    } else if (peerStore.numThriftApiErrors > 0) {
      // Running into THRIFT_API_ERROR is treated as sync completion signal.
      ++initialSyncFailureCnt;
    } else {
      // Return if there are peers still in IDLE/SYNCING state and no thrift
      // errors have occured yet.
      return;
    }
  }

  // Sync with all peers are completed.
  initialSyncCompleted_ = true;

  XLOG(INFO)
      << AreaTag() << "[Initialization] KvStore synchronization completed with "
      << fmt::format(
             "{} peers fully synced and {} peers failed with Thrift errors.",
             initialSyncSuccessCnt,
             initialSyncFailureCnt);

  // Trigger KvStore callback.
  initialKvStoreSyncedCallback_();
}

// This function will process the exception hit during full-dump:
//  1) Change peer state from current state to IDLE due to exception;
//  2) Schedule syncTimer to pick IDLE peer up if NOT scheduled;
template <class ClientType>
void
KvStoreDb<ClientType>::processThriftFailure(
    std::string const& peerName,
    folly::fbstring const& exceptionStr,
    std::chrono::milliseconds timeDelta) {
  // check if this kvStore is destructed and stop processing callbacks
  if (isStopped_) {
    return;
  }

  // check if it is valid peer(i.e. peer removed in process of syncing)
  auto it = thriftPeers_.find(peerName);
  if (it == thriftPeers_.end()) {
    return;
  }

  XLOG(INFO) << AreaTag()
             << fmt::format(
                    "Exception: {}. Processing time: {}ms.",
                    exceptionStr,
                    timeDelta.count());

  auto& peer = it->second;
  ++peer.numThriftApiErrors;
  disconnectPeer(peer, KvStorePeerEvent::THRIFT_API_ERROR);
}

template <class ClientType>
void
KvStoreDb<ClientType>::disconnectPeer(
    KvStorePeer& peer, KvStorePeerEvent const& event) {
  // we want to correct inconsistency fast
  if (event != KvStorePeerEvent::INCONSISTENCY_DETECTED) {
    peer.expBackoff.reportError(); // apply exponential backoff
  }

  // reset client to reconnect later in next batch of thriftSyncTimer_
  // scanning
  peer.plainTextClient.reset();
  if (kvParams_.enable_secure_thrift_client) {
    peer.secureClient.reset();
  }

  // state transition
  auto oldState = *peer.peerSpec.state();
  peer.peerSpec.state() = getNextState(oldState, event);

  if (oldState != *peer.peerSpec.state()) {
    peer.peerSpec.stateEpochTimeMs() = getTimeSinceEpochMs();
    if (oldState == thrift::KvStorePeerState::INITIALIZED) {
      peer.peerSpec.flaps() = *peer.peerSpec.flaps() + 1;
    }
  }
  logStateTransition(peer.nodeName, oldState, *peer.peerSpec.state());

  // Thrift error is treated as a completion signal of syncing with peer. Check
  // whether initial sync is completed.
  if (not initialSyncCompleted_) {
    processInitializationEvent();
  }

  // Schedule another round of `thriftSyncTimer_` in case it is
  // NOT scheduled.
  if (not thriftSyncTimer_->isScheduled()) {
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

template <class ClientType>
void
KvStoreDb<ClientType>::addThriftPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  // kvstore external sync over thrift port of knob enabled
  for (auto const& [peerName, newPeerSpec] : peers) {
    auto const& peerAddr = *newPeerSpec.peerAddr();

    // try to connect with peer
    auto peerIter = thriftPeers_.find(peerName);
    if (peerIter != thriftPeers_.end()) {
      XLOG(INFO) << AreaTag()
                 << fmt::format(
                        "[Peer Update] {} is updated with peerAddr: {}",
                        peerName,
                        peerAddr);

      const auto& oldPeerSpec = peerIter->second.peerSpec;
      if (*oldPeerSpec.peerAddr() != *newPeerSpec.peerAddr()) {
        // case1: peerSpec updated(i.e. parallel adjacencies can
        //        potentially have peerSpec updated by LM)
        XLOG(INFO) << AreaTag()
                   << fmt::format(
                          "[Peer Update] peerAddr is updated from: {} to: {}",
                          *oldPeerSpec.peerAddr(),
                          peerAddr);
      } else {
        // case2. new peer came up (previsously shut down ungracefully)
        XLOG(WARNING)
            << AreaTag()
            << fmt::format(
                   "[Peer Update] new peer {} comes up. Previously shutdown non-gracefully",
                   peerName);
      }
      logStateTransition(
          peerName,
          *peerIter->second.peerSpec.state(),
          thrift::KvStorePeerState::IDLE);

      peerIter->second.peerSpec = newPeerSpec; // update peerSpec
      peerIter->second.peerSpec.state() =
          thrift::KvStorePeerState::IDLE; // set IDLE initially

      peerIter->second.peerSpec.stateEpochTimeMs() = getTimeSinceEpochMs();
      peerIter->second.peerSpec.flaps() = -1;

      peerIter->second.plainTextClient.reset(); // destruct thriftClient
      if (kvParams_.enable_secure_thrift_client) {
        peerIter->second.secureClient.reset();
      }
    } else {
      // case 3: found a new peer coming up
      XLOG(INFO) << AreaTag()
                 << fmt::format(
                        "[Peer Add] {} is added with peerAddr: {}",
                        peerName,
                        peerAddr);

      KvStorePeer peer(
          peerName,
          AreaTag(),
          newPeerSpec,
          ExponentialBackoff<std::chrono::milliseconds>(
              kvParams_.syncInitialBackoff, kvParams_.syncMaxBackoff),
          kvParams_);
      peer.peerSpec.stateEpochTimeMs() = getTimeSinceEpochMs();
      peer.peerSpec.flaps() = -1;
      thriftPeers_.emplace(peerName, std::move(peer));
    }

    // create thrift client and do backoff if can't go through
    auto& thriftPeer = thriftPeers_.at(peerName);
    thriftPeer.getOrCreateThriftClient(evb_, kvParams_.maybeIpTos);
  } // for loop

  // kick off thriftSyncTimer_ if not yet to asyc process full-sync
  if (not thriftSyncTimer_->isScheduled()) {
    thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  }
}

template <class ClientType>
std::map<std::string, int64_t>
KvStoreDb<ClientType>::getCounters() const {
  std::map<std::string, int64_t> counters;

  // Add some more flat counters
  counters["kvstore.num_keys"] = kvStore_.size();
  counters["kvstore.num_peers"] = thriftPeers_.size();

  /*
   * ATTN: counter with [Area] tag has two layers of counters. For instance,
   *
   *  1) kvstore.num_expiring_keys
   *  2) kvstore.num_expiring_keys.<areaId>
   *
   * item 1) will be used for general monitoring for overall KvStore instance,
   * aka, alerting threshold setting;
   * item 2) will be used to peek into specific module for debugging purpose;
   */
  auto maybeNumFloodPeers = fb303::fbData->getCounterIfExists(
      fmt::format("kvstore.num_flood_peers.{}.sum.60", area_));
  auto maybeNumExpiringKeys = fb303::fbData->getCounterIfExists(
      fmt::format("kvstore.num_expiring_keys.{}.sum.60", area_));
  counters["kvstore.num_flood_peers"] =
      maybeNumFloodPeers.hasValue() ? maybeNumFloodPeers.value() : 0;
  counters["kvstore.num_expiring_keys"] =
      maybeNumExpiringKeys.hasValue() ? maybeNumExpiringKeys.value() : 0;

  return counters;
}

template <class ClientType>
void
KvStoreDb<ClientType>::delThriftPeers(std::vector<std::string> const& peers) {
  for (auto const& peerName : peers) {
    auto peerIter = thriftPeers_.find(peerName);
    if (peerIter == thriftPeers_.end()) {
      XLOG(ERR)
          << AreaTag()
          << fmt::format(
                 "[Peer Delete] try to delete non-existing peer: {}. Skip.",
                 peerName);
      continue;
    }
    const auto& peerSpec = peerIter->second.peerSpec;

    XLOG(INFO) << AreaTag()
               << fmt::format(
                      "[Peer Delete] {} is detached from peerAddr: {}",
                      peerName,
                      *peerSpec.peerAddr());

    // destroy peer info
    peerIter->second.plainTextClient.reset();
    if (kvParams_.enable_secure_thrift_client) {
      peerIter->second.secureClient.reset();
    }
    thriftPeers_.erase(peerIter);
  }
}

// dump all peers we are subscribed to
template <class ClientType>
thrift::PeersMap
KvStoreDb<ClientType>::dumpPeers() {
  thrift::PeersMap peers;
  for (auto& [peerName, thriftPeer] : thriftPeers_) {
    thriftPeer.peerSpec.stateElapsedTimeMs() =
        getTimeSinceEpochMs() - *thriftPeer.peerSpec.stateEpochTimeMs();

    peers.emplace(peerName, thriftPeer.peerSpec);
  }
  return peers;
}

template <class ClientType>
void
KvStoreDb<ClientType>::cleanupTtlCountdownQueue() {
  // record all expired keys
  std::vector<std::string> expiredKeys;
  auto now = std::chrono::steady_clock::now();

  // Iterate through ttlCountdownQueue_ until the top expires in the future
  while (not ttlCountdownQueue_.empty()) {
    auto top = ttlCountdownQueue_.top();
    if (top.expiryTime > now) {
      // Nothing in queue worth evicting
      break;
    }
    auto it = kvStore_.find(top.key);
    if (it != kvStore_.end() and *it->second.version() == top.version and
        *it->second.originatorId() == top.originatorId and
        *it->second.ttlVersion() == top.ttlVersion) {
      expiredKeys.emplace_back(top.key);
      XLOG(WARNING)
          << AreaTag()
          << "Delete expired (key, version, originatorId, ttlVersion, ttl, node) "
          << fmt::format(
                 "({}, {}, {}, {}, {}, {})",
                 top.key,
                 *it->second.version(),
                 *it->second.originatorId(),
                 *it->second.ttlVersion(),
                 *it->second.ttl(),
                 kvParams_.nodeId);
      logKvEvent("KEY_EXPIRE", top.key);
      kvStore_.erase(it);
    }
    ttlCountdownQueue_.pop();
  }

  // Reschedule based on most recent timeout
  if (not ttlCountdownQueue_.empty()) {
    ttlCountdownTimer_->scheduleTimeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ttlCountdownQueue_.top().expiryTime - now));
  }

  if (expiredKeys.empty()) {
    // no key expires
    return;
  }

  fb303::fbData->addStatValue(
      "kvstore.expired_key_vals", expiredKeys.size(), fb303::SUM);

  // ATTN: expired key will be ONLY notified to local subscribers
  //       via replicate-queue. KvStore will NOT flood publication
  //       with expired keys ONLY to external peers.
  thrift::Publication expiredKeysPub{};
  expiredKeysPub.expiredKeys() = std::move(expiredKeys);
  expiredKeysPub.area() = area_;
  floodPublication(std::move(expiredKeysPub));
}

template <class ClientType>
void
KvStoreDb<ClientType>::bufferPublication(thrift::Publication&& publication) {
  fb303::fbData->addStatValue("kvstore.rate_limit_suppress", 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.rate_limit_keys", publication.keyVals()->size(), fb303::AVG);
  std::optional<std::string> floodRootId{std::nullopt};
  // update or add keys
  for (auto const& [key, _] : *publication.keyVals()) {
    publicationBuffer_[floodRootId].emplace(key);
  }
  for (auto const& key : *publication.expiredKeys()) {
    publicationBuffer_[floodRootId].emplace(key);
  }
}

template <class ClientType>
void
KvStoreDb<ClientType>::floodBufferedUpdates() {
  if (!publicationBuffer_.size()) {
    return;
  }

  // merged-publications to be sent
  std::vector<thrift::Publication> publications;

  // merge publication per root-id
  for (const auto& [rootId, keys] : publicationBuffer_) {
    thrift::Publication publication{};
    for (const auto& key : keys) {
      auto kvStoreIt = kvStore_.find(key);
      if (kvStoreIt != kvStore_.end()) {
        publication.keyVals()->emplace(make_pair(key, kvStoreIt->second));
      } else {
        publication.expiredKeys()->emplace_back(key);
      }
    }
    publications.emplace_back(std::move(publication));
  }

  publicationBuffer_.clear();

  for (auto& pub : publications) {
    // when sending out merged publication, we maintain orginal-root-id
    // we act as a forwarder, NOT an initiator.
    floodPublication(std::move(pub), false /* rate-limit */);
  }
}

template <class ClientType>
void
KvStoreDb<ClientType>::finalizeFullSync(
    const std::unordered_set<std::string>& keys, const std::string& senderId) {
  // build keyval to be sent
  thrift::Publication updates;
  for (const auto& key : keys) {
    const auto& it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      updates.keyVals()->emplace(key, it->second);
    }
  }

  // Update ttl values to remove expiring keys. Ignore the response if no
  // keys to be sent
  updatePublicationTtl(ttlCountdownQueue_, kvParams_.ttlDecr, updates);
  if (not updates.keyVals()->size()) {
    return;
  }

  // Build params for final sync of 3-way handshake
  thrift::KeySetParams params;
  params.keyVals() = std::move(*updates.keyVals());
  params.timestamp_ms() = getUnixTimeStampMs();
  params.nodeIds() = {kvParams_.nodeId};

  auto peerIt = thriftPeers_.find(senderId);
  if (peerIt == thriftPeers_.end()) {
    XLOG(ERR)
        << AreaTag()
        << fmt::format(
               "[Thrift Sync] Invalid peer: {} to do finalize sync with. Skip it.",
               senderId);
    return;
  }

  auto& thriftPeer = peerIt->second;
  if (*thriftPeer.peerSpec.state() == thrift::KvStorePeerState::IDLE or
      ((not thriftPeer.plainTextClient)) or
      (kvParams_.enable_secure_thrift_client and not thriftPeer.secureClient)) {
    // TODO: evaluate the condition later to add to pending collection
    // peer in thriftPeers collection can still be in IDLE state.
    // Skip final full-sync with those peers.
    return;
  }
  params.senderId() = kvParams_.nodeId;
  XLOG(INFO) << AreaTag()
             << fmt::format(
                    "[Thrift Sync] Finalize full-sync back to: {} with {} keys",
                    senderId,
                    keys.size());

  // record telemetry for thrift calls
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_finalized_sync", 1, fb303::COUNT);

  auto startTime = std::chrono::steady_clock::now();

  params.timestamp_ms() = std::chrono::duration_cast<std::chrono::milliseconds>(
                              startTime.time_since_epoch())
                              .count();

  auto sf = thriftPeer.setKvStoreKeyValsWrapper(area_, params);
  std::move(sf)
      .via(evb_->getEvb())
      .thenValue([this, senderId, startTime](folly::Unit&&) {
        XLOG(DBG4)
            << AreaTag()
            << fmt::format(
                   "[Thrift Sync] Finalize full-sync ack received from peer: {}",
                   senderId);

        auto endTime = std::chrono::steady_clock::now();
        auto timeDelta = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime);

        // record telemetry for thrift calls
        fb303::fbData->addStatValue(
            "kvstore.thrift.num_finalized_sync_success", 1, fb303::COUNT);
        fb303::fbData->addStatValue(
            "kvstore.thrift.finalized_sync_duration_ms",
            timeDelta.count(),
            fb303::AVG);
      })
      .thenError([this, senderId, startTime](
                     const folly::exception_wrapper& ew) {
        // state transition to IDLE
        auto endTime = std::chrono::steady_clock::now();
        auto timeDelta = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime);
        processThriftFailure(
            senderId,
            fmt::format(
                "Finalized FULL_SYNC failure with {}, {}", senderId, ew.what()),
            timeDelta);

        // record telemetry for thrift calls
        fb303::fbData->addStatValue(
            "kvstore.thrift.num_finalized_sync_failure", 1, fb303::COUNT);
      });
}

template <class ClientType>
std::unordered_set<std::string>
KvStoreDb<ClientType>::getFloodPeers() {
  // flood-peers:
  //  1) SPT-peers;
  //  2) peers-who-does-not-support-DUAL;
  std::unordered_set<std::string> floodPeers;
  for (const auto& [peerName, peer] : thriftPeers_) {
    floodPeers.emplace(peerName);
  }
  return floodPeers;
}

template <class ClientType>
void
KvStoreDb<ClientType>::floodPublication(
    thrift::Publication&& publication, bool rateLimit) {
  // rate limit if configured
  if (floodLimiter_ && rateLimit && !floodLimiter_->consume(1)) {
    bufferPublication(std::move(publication));
    pendingPublicationTimer_->scheduleTimeout(
        Constants::kFloodPendingPublication);
    return;
  }
  // merge with buffered publication and flood
  if (publicationBuffer_.size()) {
    bufferPublication(std::move(publication));
    return floodBufferedUpdates();
  }
  // Update ttl on keys we are trying to advertise. Also remove keys which
  // are about to expire.
  updatePublicationTtl(ttlCountdownQueue_, kvParams_.ttlDecr, publication);

  // If there are no changes then return
  if (publication.keyVals()->empty() && publication.expiredKeys()->empty()) {
    return;
  }

  // Find from whom we might have got this publication. Last entry is our ID
  // and hence second last entry is the node from whom we get this
  // publication
  std::optional<std::string> senderId;
  if (publication.nodeIds().has_value() and publication.nodeIds()->size()) {
    senderId = publication.nodeIds()->back();
  }
  if (not publication.nodeIds().has_value()) {
    publication.nodeIds() = std::vector<std::string>{};
  }
  publication.nodeIds()->emplace_back(kvParams_.nodeId);

  // Flood publication to internal subscribers
  kvParams_.kvStoreUpdatesQueue.push(publication);
  fb303::fbData->addStatValue("kvstore.num_updates", 1, fb303::COUNT);

  // Process potential update to self-originated key-vals
  processPublicationForSelfOriginatedKey(publication);

  // Flood keyValue ONLY updates to external neighbors
  if (publication.keyVals()->empty()) {
    return;
  }

  // Key collection to be flooded
  auto keysToUpdate = folly::gen::from(*publication.keyVals()) |
      folly::gen::get<0>() | folly::gen::as<std::vector<std::string>>();

  XLOG(DBG2) << AreaTag()
             << fmt::format(
                    "Flood publication from: {} to peers with: {} key-vals. ",
                    kvParams_.nodeId,
                    keysToUpdate.size())
             << fmt::format("Updated keys: {}", folly::join(",", keysToUpdate));

  // prepare thrift structure for flooding purpose
  thrift::KeySetParams params;
  params.keyVals() = *publication.keyVals();
  params.nodeIds().copy_from(publication.nodeIds());
  params.timestamp_ms() = getUnixTimeStampMs();
  params.senderId() = kvParams_.nodeId;

  for (auto& [peerName, thriftPeer] : thriftPeers_) {
    if (senderId.has_value() and senderId.value() == peerName) {
      // Do not flood towards senderId from whom we received this
      // publication
      continue;
    }

    if (*thriftPeer.peerSpec.state() != thrift::KvStorePeerState::INITIALIZED) {
      // Skip flooding to those peers if peer has NOT finished
      // initial sync(i.e. promoted to `INITIALIZED`)
      // store key for flooding after intialized
      for (auto const& [key, _] : *params.keyVals()) {
        thriftPeer.pendingKeysDuringInitialization.insert(key);
      }
      continue;
    }

    // record telemetry for flooding publications
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_flood_pub", 1, fb303::COUNT);
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_flood_key_vals",
        publication.keyVals()->size(),
        fb303::SUM);

    auto startTime = std::chrono::steady_clock::now();
    auto sf = thriftPeer.setKvStoreKeyValsWrapper(area_, params);
    std::move(sf)
        .via(evb_->getEvb())
        .thenValue([startTime](folly::Unit&&) {
          auto endTime = std::chrono::steady_clock::now();
          auto timeDelta =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  endTime - startTime);

          // record telemetry for thrift calls
          fb303::fbData->addStatValue(
              "kvstore.thrift.num_flood_pub_success", 1, fb303::COUNT);
          fb303::fbData->addStatValue(
              "kvstore.thrift.flood_pub_duration_ms",
              timeDelta.count(),
              fb303::AVG);
        })
        .thenError([this, peerNameStr = peerName, startTime](
                       const folly::exception_wrapper& ew) {
          // state transition to IDLE
          auto endTime = std::chrono::steady_clock::now();
          auto timeDelta =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  endTime - startTime);
          processThriftFailure(
              peerNameStr,
              fmt::format(
                  "FLOOD_PUB failure with {}, {}", peerNameStr, ew.what()),
              timeDelta);

          // record telemetry for thrift calls
          fb303::fbData->addStatValue(
              "kvstore.thrift.num_flood_pub_failure", 1, fb303::COUNT);
        });
  }
}

template <class ClientType>
void
KvStoreDb<ClientType>::processPublicationForSelfOriginatedKey(
    thrift::Publication const& publication) {
  // direct return to avoid performance issue
  if (selfOriginatedKeyVals_.empty()) {
    return;
  }

  // go through received publications to refresh self-originated key-vals if
  // necessary
  for (auto const& [key, rcvdValue] : *publication.keyVals()) {
    if (not rcvdValue.value().has_value()) {
      // ignore TTL update
      continue;
    }

    // update local self-originated key-vals
    auto it = selfOriginatedKeyVals_.find(key);
    if (it == selfOriginatedKeyVals_.end()) {
      // skip processing since it is none of our interest
      continue;
    }

    // 3 cases to process for version comparison
    //
    // case-1: currValue > rcvdValue
    // case-2: currValue < rcvdValue
    // case-3: currValue == rcvdValue
    auto& currValue = it->second.value;
    const auto& currVersion = *currValue.version();
    const auto& rcvdVersion = *rcvdValue.version();
    bool shouldOverride{false};

    if (currVersion > rcvdVersion) {
      // case-1: ignore rcvdValue since it is "older" than local keys.
      continue;
    } else if (currVersion < rcvdVersion) {
      // case-2: rcvdValue has higher version, MUST override.
      shouldOverride = true;
    } else {
      // case-3: currValue has the SAME version as rcvdValue,
      // conditionally override.
      // NOTE: similar operation in persistSelfOriginatedKey()
      // for key overriding.
      if (*rcvdValue.originatorId() != kvParams_.nodeId or
          *currValue.value() != *rcvdValue.value()) {
        shouldOverride = true;
      }
    }

    // NOTE: local KvStoreDb needs to override and re-advertise, including:
    //  - bump up version;
    //  - reset ttlVersion;
    //  - override originatorId(do nothing since it is up-to-date);
    //  - override value(do nothing since it is up-to-date);
    //  - honor the ttl from local value;
    if (shouldOverride) {
      currValue.ttlVersion() = 0;
      currValue.version() = *rcvdValue.version() + 1;
      keysToAdvertise_.insert(key);

      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "Override version for [key: {}, v: {}, originatorId: {}]",
                 key,
                 *rcvdValue.version(),
                 *currValue.originatorId());
    } else {
      // update local ttlVersion if received higher ttlVersion.
      // NOTE: ttlVersion will be bumped up before ttl update.
      // It works fine to just update to latest ttlVersion, instead of +1.
      if (*currValue.ttlVersion() < *rcvdValue.ttlVersion()) {
        currValue.ttlVersion() = *rcvdValue.ttlVersion();
        scheduleTtlUpdates(key, true /* advertiseImmediately*/);
      }
    }
  }

  // NOTE: use throttling to NOT block publication flooding.
  advertiseSelfOriginatedKeysThrottled_->operator()();

  // TODO: when native key subscription is supported. Handle callback here.
}

template <class ClientType>
thrift::KvStoreMergeResult
KvStoreDb<ClientType>::mergePublication(
    thrift::Publication const& rcvdPublication,
    bool isSelfOriginatedUpdate,
    std::optional<std::string> senderId) {
  // keyVals is NOT an optional field. Use & to avoid copy.
  const auto& keyVals = *rcvdPublication.keyVals();
  const auto tobeUpdatedKeys = rcvdPublication.tobeUpdatedKeys();
  const auto nodeIds = rcvdPublication.nodeIds();

  /*
   * [Loop Prevention]
   *
   * KvStoreDb will add itself to the "visited" path to prevent looping.
   *
   */
  if (nodeIds.has_value() and
      std::find(nodeIds->cbegin(), nodeIds->cend(), kvParams_.nodeId) !=
          nodeIds->cend()) {
    fb303::fbData->addStatValue("kvstore.looped_publications", 1, fb303::COUNT);
    thrift::KvStoreMergeResult result;
    for (const auto& [key, _] : keyVals) {
      result.noMergeKeyVals()->emplace(
          std::make_pair(key, thrift::KvStoreNoMergeReason::LOOP_DETECTED));
    }
    return result;
  }

  if (not isSelfOriginatedUpdate) {
    /*
     * NOTE: received* counters are ONLY used for publication received remotely.
     * Update/publication for self-originated keys will be excluded.
     */
    fb303::fbData->addStatValue(
        "kvstore.received_publications", 1, fb303::COUNT);
    fb303::fbData->addStatValue(
        "kvstore.received_publications." + area_, 1, fb303::COUNT);
    fb303::fbData->addStatValue(
        "kvstore.received_key_vals", keyVals.size(), fb303::SUM);
    fb303::fbData->addStatValue(
        "kvstore.received_key_vals." + area_, keyVals.size(), fb303::SUM);
  }

  /*
   * [3-way Sync]
   *
   * https://openr.readthedocs.io/Protocol_Guide/KvStore.html#finalized-full-sync-part-of-3-way-sync
   *
   * Prepare keys to send back to sender if both conditions are met:
   *  1) senderId is non-empty. This happens ONLY during full-sync.
   *  2) received publication(from peer) has non-empty `tobeUpdatedKeys`.
   *     This will happen with two cases:
   *
   *     i) peer does NOT have this key
   *     ii) peer's version is lower.
   */
  std::unordered_set<std::string> keysToSendBack{};
  if (senderId.has_value()) {
    XLOG(DBG3)
        << AreaTag()
        << fmt::format(
               "Received publication from {} with: {} keyVals, {} tobeUpdatedKeys",
               senderId.value(),
               keyVals.size(),
               tobeUpdatedKeys.has_value() ? tobeUpdatedKeys->size() : 0);

    // retrieve keys from peer's request
    if (tobeUpdatedKeys.has_value()) {
      for (const auto& key : *tobeUpdatedKeys) {
        keysToSendBack.insert(key);
      }
    }

    // merge with pending keys due to peer NOT in INITIALIZED state
    auto peerIt = thriftPeers_.find(senderId.value());
    if (peerIt != thriftPeers_.end()) {
      keysToSendBack.merge(peerIt->second.pendingKeysDuringInitialization);
      peerIt->second.pendingKeysDuringInitialization.clear();
    }
  }

  /*
   * ATTN: This can happen when KvStore is emitting expired-key updates
   *
   * When KvStore is emitting expiredKeys, expiredKeys will NOT be flooded as
   * delta to peers to prevent endless bouncing back of key add/expiration.
   */
  if (keyVals.empty() and keysToSendBack.empty()) {
    thrift::KvStoreMergeResult result;
    return result;
  }

  /*
   * [Merge with Local Store]
   *
   *  - `rcvdPublication.keyVals` will be merged with local kvStore_
   *  - delta will be generated with the form of `thrift::Publication`
   *  - delta will be flooded to peers
   *
   * ATTN: sender is used for version inconsistency detection.
   */
  auto sender = senderId.has_value()
      ? senderId
      : (nodeIds.has_value() ? std::optional(nodeIds->back()) : std::nullopt);

  const auto result =
      mergeKeyValues(kvStore_, keyVals, kvParams_.filters, sender);
  const auto& mergedKeyVals = *result.keyVals();
  if (*result.inconsistencyDetetectedWithOriginator()) {
    // inconsistency detected: Received a TTL update from originator
    // but key version are mismatched
    // Transition to IDLE to resync
    auto it = thriftPeers_.find(sender.value());
    if (it != thriftPeers_.end()) {
      disconnectPeer(it->second, KvStorePeerEvent::INCONSISTENCY_DETECTED);
      fb303::fbData->addStatValue(
          "kvstore.num_conflict_version_key", 1, fb303::COUNT);
      return result;
    }
  }

  /*
   * Prepare thrift::Publication structure with updated key-value pairs
   */
  thrift::Publication deltaPublication;
  deltaPublication.keyVals() = std::move(mergedKeyVals);
  deltaPublication.area() = area_;

  const size_t kvUpdateCnt = deltaPublication.keyVals()->size();
  fb303::fbData->addStatValue(
      "kvstore.updated_key_vals", kvUpdateCnt, fb303::SUM);
  fb303::fbData->addStatValue(
      "kvstore.updated_key_vals." + area_, kvUpdateCnt, fb303::SUM);

  // Populate nodeIds. ATTN: nodeId of itself will be appended later
  // inside `floodPublication`
  if (nodeIds.has_value()) {
    deltaPublication.nodeIds().copy_from(rcvdPublication.nodeIds());
  }

  // Update ttl values of keys
  updateTtlCountdownQueue(deltaPublication, isSelfOriginatedUpdate);

  if (not deltaPublication.keyVals()->empty()) {
    // Flood change to all of our neighbors/subscribers
    floodPublication(std::move(deltaPublication));
  } else {
    // Keep track of received publications which din't update any field
    fb303::fbData->addStatValue(
        "kvstore.received_redundant_publications", 1, fb303::COUNT);
  }

  // response to senderId with tobeUpdatedKeys + Vals
  // (last step in 3-way full-sync)
  if (not keysToSendBack.empty()) {
    finalizeFullSync(keysToSendBack, senderId.value());
  }

  return result;
}

template <class ClientType>
void
KvStoreDb<ClientType>::logSyncEvent(
    const std::string& peerNodeName,
    const std::chrono::milliseconds syncDuration) {
  LogSample sample{};

  sample.addString("area", AreaTag());
  sample.addString("event", "KVSTORE_FULL_SYNC");
  sample.addString("node_name", kvParams_.nodeId);
  sample.addString("neighbor", peerNodeName);
  sample.addInt("duration_ms", syncDuration.count());

  kvParams_.logSampleQueue.push(std::move(sample));
}

template <class ClientType>
void
KvStoreDb<ClientType>::logKvEvent(
    const std::string& event, const std::string& key) {
  LogSample sample{};

  sample.addString("area", AreaTag());
  sample.addString("event", event);
  sample.addString("node_name", kvParams_.nodeId);
  sample.addString("key", key);

  kvParams_.logSampleQueue.push(std::move(sample));
}

/* Coroutines */
#if FOLLY_HAS_COROUTINES
template <class ClientType>
folly::coro::Task<thrift::Publication>
KvStore<ClientType>::co_getKvStoreKeyValsInternal(
    std::string area, thrift::KeyGetParams keyGetParams) {
  auto& kvStoreDb = getAreaDbOrThrow(std::move(area), "getKvStoreKeyVals");
  auto thriftPub = kvStoreDb.getKeyVals(*keyGetParams.keys());
  updatePublicationTtl(
      kvStoreDb.getTtlCountdownQueue(), kvParams_.ttlDecr, thriftPub, false);
  co_return thriftPub;
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<std::vector<thrift::Publication>>>
KvStore<ClientType>::co_dumpKvStoreKeysImpl(
    thrift::KeyDumpParams keyDumpParams, std::set<std::string> selectAreas) {
  auto result =
      dumpKvStoreKeysImpl(std::move(keyDumpParams), std::move(selectAreas));
  co_return result;
}

template <class ClientType>
folly::coro::Task<thrift::Publication>
KvStore<ClientType>::co_dumpKvStoreHashesImpl(
    std::string area, thrift::KeyDumpParams keyDumpParams) {
  auto result =
      dumpKvStoreHashesImpl(std::move(area), std::move(keyDumpParams));
  co_return result;
}

template <class ClientType>
folly::coro::Task<std::vector<thrift::KvStoreAreaSummary>>
KvStore<ClientType>::co_getKvStoreAreaSummaryImpl(
    std::set<std::string> selectAreas) {
  auto result = getKvStoreAreaSummaryImpl(std::move(selectAreas));
  co_return result;
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<thrift::Publication>>
KvStore<ClientType>::co_getKvStoreKeyVals(
    std::string area, thrift::KeyGetParams keyGetParams) {
  XLOG(DBG3) << "Get key requested for AREA: " << area;
  try {
    auto result =
        co_await co_getKvStoreKeyValsInternal(area, std::move(keyGetParams))
            .scheduleOn(getEvb());
    co_return std::make_unique<thrift::Publication>(std::move(result));
  } catch (thrift::KvStoreError const& e) {
    XLOG(ERR) << fmt::format(
        "{} got exception: {} for area {}", __FUNCTION__, e.what(), area);
    throw e;
  }
  co_return std::make_unique<thrift::Publication>();
}

template <class ClientType>
folly::coro::Task<thrift::SetKeyValsResult>
KvStore<ClientType>::co_setKvStoreKeyValsInternal(
    std::string area, thrift::KeySetParams keySetParams) {
  auto& kvStoreDb = getAreaDbOrThrow(std::move(area), "setKvStoreKeyVals");
  auto result =
      kvStoreDb.setKeyVals(std::move(keySetParams), false /* remote update */);
  co_return result;
}

template <class ClientType>
folly::coro::Task<folly::Unit>
KvStore<ClientType>::co_setKvStoreKeyVals(
    std::string area, thrift::KeySetParams keySetParams) {
  XLOG(DBG3) << fmt::format(
      "Set key requested for AREA: {}, by sender: {}, at time: {}",
      area,
      (keySetParams.senderId().has_value() ? keySetParams.senderId().value()
                                           : ""),
      (keySetParams.timestamp_ms().has_value()
           ? folly::to<std::string>(keySetParams.timestamp_ms().value())
           : ""));
  try {
    auto result =
        co_await co_setKvStoreKeyValsInternal(area, std::move(keySetParams))
            .scheduleOn(getEvb());
  } catch (thrift::KvStoreError const& e) {
    XLOG(ERR) << fmt::format(
        "{} got exception: {} for area {}", __FUNCTION__, e.what(), area);
    throw;
  }
  co_return folly::Unit();
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<thrift::SetKeyValsResult>>
KvStore<ClientType>::co_setKvStoreKeyValues(
    std::string area, thrift::KeySetParams keySetParams) {
  XLOG(DBG3) << fmt::format(
      "Set key requested for AREA: {}, by sender: {}, at time: {}",
      area,
      (keySetParams.senderId().has_value() ? keySetParams.senderId().value()
                                           : ""),
      (keySetParams.timestamp_ms().has_value()
           ? folly::to<std::string>(keySetParams.timestamp_ms().value())
           : ""));
  try {
    auto result =
        co_await co_setKvStoreKeyValsInternal(area, std::move(keySetParams))
            .scheduleOn(getEvb());
    co_return std::make_unique<thrift::SetKeyValsResult>(result);
  } catch (thrift::KvStoreError const& e) {
    XLOG(ERR) << fmt::format(
        "{} got exception: {} for area {}", __FUNCTION__, e.what(), area);
    throw;
  }
  co_return std::make_unique<thrift::SetKeyValsResult>();
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<std::vector<thrift::Publication>>>
KvStore<ClientType>::co_dumpKvStoreKeys(
    thrift::KeyDumpParams keyDumpParams, std::set<std::string> selectAreas) {
  auto result = co_await co_dumpKvStoreKeysImpl(
                    std::move(keyDumpParams), std::move(selectAreas))
                    .scheduleOn(getEvb());
  co_return result;
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<thrift::Publication>>
KvStore<ClientType>::co_dumpKvStoreHashes(
    std::string area, thrift::KeyDumpParams keyDumpParams) {
  auto result = co_await co_dumpKvStoreHashesImpl(
                    std::move(area), std::move(keyDumpParams))
                    .scheduleOn(getEvb());
  co_return std::make_unique<thrift::Publication>(result);
}

template <class ClientType>
folly::coro::Task<thrift::Publication>
KvStoreDb<ClientType>::KvStorePeer::getKvStoreKeyValsFilteredAreaCoroWrapper(
    const thrift::KeyDumpParams& filter, const std::string& area) {
  if (not kvParams_.enable_secure_thrift_client) {
    co_return co_await plainTextClient->co_getKvStoreKeyValsFilteredArea(
        filter, area);
  }
  // TLS fallback
  try {
    co_return co_await secureClient->co_getKvStoreKeyValsFilteredArea(
        filter, area);
  } catch (const folly::AsyncSocketException& ex) {
    XLOG(ERR) << fmt::format("{} got exception: {}", __FUNCTION__, ex.what());
    fb303::fbData->addStatValue(
        "kvstore.thrift.co_getKvStoreKeyValsFilteredArea.secure_client.failure",
        1,
        fb303::COUNT);
    co_return plainTextClient->co_getKvStoreKeyValsFilteredArea(filter, area);
  }
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>>
KvStore<ClientType>::co_getKvStoreAreaSummaryInternal(
    std::set<std::string> selectAreas) {
  auto result = co_await co_getKvStoreAreaSummaryImpl(std::move(selectAreas))
                    .scheduleOn(getEvb());
  co_return std::make_unique<std::vector<thrift::KvStoreAreaSummary>>(result);
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<thrift::PeersMap>>
KvStore<ClientType>::co_getKvStorePeers(std::string area) {
  XLOG(DBG2) << fmt::format("Peer dump requested for AREA: {}", area);
  fb303::fbData->addStatValue("kvstore.cmd_peer_dump", 1, fb303::COUNT);
  auto result = getAreaDbOrThrow(area, "co_getKvStorePeers").dumpPeers();
  co_return std::make_unique<thrift::PeersMap>(result);
}
#endif // FOLLY_HAS_COROUTINES
} // namespace openr
