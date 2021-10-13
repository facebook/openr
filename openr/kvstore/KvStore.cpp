/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/GLog.h>
#include <folly/Random.h>
#include <folly/String.h>
#include <folly/logging/xlog.h>
#include <openr/common/Util.h>

#include <openr/common/Constants.h>
#include <openr/common/EventLogger.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>
#include <openr/kvstore/KvStore.h>

using namespace std::chrono;

namespace fb303 = facebook::fb303;

namespace openr {

KvStore::KvStore(
    // initializers for immutable state
    fbzmq::Context& zmqContext,
    messaging::ReplicateQueue<Publication>& kvStoreUpdatesQueue,
    messaging::ReplicateQueue<KvStoreSyncEvent>& kvStoreSyncEventsQueue,
    messaging::RQueue<PeerEvent> peerUpdatesQueue,
    messaging::RQueue<KeyValueRequest> kvRequestQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    KvStoreGlobalCmdUrl globalCmdUrl,
    std::shared_ptr<const Config> config)
    : kvParams_(
          config->getNodeName(),
          kvStoreUpdatesQueue,
          kvStoreSyncEventsQueue,
          logSampleQueue,
          fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>(
              zmqContext,
              fbzmq::IdentityString{
                  fmt::format("{}::TCP::CMD", config->getNodeName())},
              folly::none,
              fbzmq::NonblockingFlag{true}),
          config->getKvStoreConfig().get_zmq_hwm(),
          getKvStoreFilters(config),
          config->getKvStoreConfig().flood_rate_ref().to_optional(),
          std::chrono::milliseconds(
              *config->getKvStoreConfig().ttl_decrement_ms_ref()),
          std::chrono::milliseconds(
              *config->getKvStoreConfig().key_ttl_ms_ref()),
          config->getKvStoreConfig().enable_flood_optimization_ref().value_or(
              false),
          config->getKvStoreConfig().is_flood_root_ref().value_or(false),
          config->getKvStoreConfig().get_enable_thrift_dual_msg(),
          config->getConfig().get_enable_kvstore_request_queue()) {
  // Schedule periodic timer for counters submission
  counterUpdateTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    for (auto& [key, val] : getGlobalCounters()) {
      fb303::fbData->setCounter(key, val);
    }
    counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);
  });
  counterUpdateTimer_->scheduleTimeout(Constants::kCounterSubmitInterval);

  // Get optional ip_tos from the config
  std::optional<int> maybeIpTos = std::nullopt;
  auto ipTosConfig = config->getConfig().ip_tos_ref();
  if (ipTosConfig.has_value()) {
    maybeIpTos = ipTosConfig.value();
  }
  kvParams_.maybeIpTos = maybeIpTos;

  // [TO BE DEPRECATED]
  if (kvParams_.enableFloodOptimization) {
    // Prepare global command socket
    prepareSocket(
        kvParams_.globalCmdSock, std::string(globalCmdUrl), maybeIpTos);
    addSocket(
        fbzmq::RawZmqSocketPtr{*kvParams_.globalCmdSock},
        ZMQ_POLLIN,
        [this](int) noexcept {
          // Drain all available messages in loop
          while (true) {
            // NOTE: globalCmSock is connected with neighbor's peerSyncSock_.
            // recvMultiple() will get a vector of fbzmq::Message which has:
            //  1) requestIdMsg; 2) delimMsg; 3) kvStoreRequestMsg;
            auto maybeReq = kvParams_.globalCmdSock.recvMultiple();
            if (maybeReq.hasError() and maybeReq.error().errNum == EAGAIN) {
              break;
            }

            if (maybeReq.hasError()) {
              XLOG(ERR) << "failed reading messages from globalCmdSock: "
                        << maybeReq.error();
              continue;
            }

            processCmdSocketRequest(std::move(maybeReq).value());
          } // while
        });
  }

  // Add reader to process peer updates from LinkMonitor
  addFiberTask([q = std::move(peerUpdatesQueue), this]() mutable noexcept {
    XLOG(INFO) << "Starting peer updates processing fiber";
    while (true) {
      auto maybePeerUpdate = q.get(); // perform read
      XLOG(DBG2) << "Received peer update...";
      if (maybePeerUpdate.hasError()) {
        XLOG(INFO) << "Terminating peer updates processing fiber";
        break;
      }
      try {
        processPeerUpdates(std::move(maybePeerUpdate).value());
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Failed to process peer request. Exception: " << ex.what();
      }
    }
  });

  // Add reader to process key-value requests from PrefixManager and LinkMonitor
  if (kvParams_.enableKvStoreRequestQueue) {
    addFiberTask(
        [kvQueue = std::move(kvRequestQueue), this]() mutable noexcept {
          XLOG(INFO) << "Starting key-value requests processing fiber";
          while (true) {
            auto maybeKvRequest = kvQueue.get(); // perform read
            XLOG(DBG2) << "Received key-value request...";
            if (maybeKvRequest.hasError()) {
              XLOG(INFO) << "Terminating key-value request processing fiber";
              break;
            }
            try {
              processKeyValueRequest(std::move(maybeKvRequest).value());
            } catch (const std::exception& ex) {
              XLOG(ERR) << "Failed to process key-value request. Exception: "
                        << ex.what();
            }
          }
        });
  }

  initGlobalCounters();

  // create KvStoreDb instances
  for (auto const& area : config->getAreaIds()) {
    kvStoreDb_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(area),
        std::forward_as_tuple(
            this,
            kvParams_,
            area,
            fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT>(
                zmqContext,
                fbzmq::IdentityString{folly::to<std::string>(
                    config->getNodeName(), "::TCP::SYNC::", area)},
                folly::none,
                fbzmq::NonblockingFlag{true}),
            config->getKvStoreConfig().is_flood_root_ref().value_or(false),
            config->getNodeName(),
            std::bind(&KvStore::initialKvStoreDbSynced, this)));
  }
}

void
KvStore::stop() {
  getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    // NOTE: destructor of every instance inside `kvStoreDb_` will gracefully
    //       exit and wait for all pending thrift requests to be processed
    //       before eventbase stops.
    for (auto& [area, kvDb] : kvStoreDb_) {
      kvDb.stop();
    }
  });

  // Invoke stop method of super class
  OpenrEventBase::stop();
  XLOG(DBG1) << "KvStore event base stopped";
}

void
KvStore::prepareSocket(
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& socket,
    std::string const& url,
    std::optional<int> maybeIpTos) {
  std::vector<std::pair<int, int>> socketOptions{
      {ZMQ_SNDHWM, Constants::kHighWaterMark},
      {ZMQ_RCVHWM, Constants::kHighWaterMark},
      {ZMQ_SNDTIMEO, Constants::kReadTimeout.count()},
      {ZMQ_ROUTER_HANDOVER, 1},
      {ZMQ_TCP_KEEPALIVE, Constants::kKeepAliveEnable},
      {ZMQ_TCP_KEEPALIVE_IDLE, Constants::kKeepAliveTime.count()},
      {ZMQ_TCP_KEEPALIVE_CNT, Constants::kKeepAliveCnt},
      {ZMQ_TCP_KEEPALIVE_INTVL, Constants::kKeepAliveIntvl.count()}};

  if (maybeIpTos.has_value()) {
    socketOptions.emplace_back(ZMQ_TOS, maybeIpTos.value());
  }

  for (const auto& [opt, val] : socketOptions) {
    auto rc = socket.setSockOpt(opt, &val, sizeof(val));
    if (rc.hasError()) {
      XLOG(ERR) << "Error setting zmq opt: " << opt << "to " << val
                << ". Error: " << rc.error();
    }
  }

  auto rc = socket.bind(fbzmq::SocketUrl{url});
  if (rc.hasError()) {
    XLOG(ERR) << "Error binding to URL '" << url << "'. Error: " << rc.error();
  }
}

KvStoreDb&
KvStore::getAreaDbOrThrow(
    std::string const& areaId, std::string const& caller) {
  auto search = kvStoreDb_.find(areaId);
  if (kvStoreDb_.end() == search) {
    XLOG(WARNING) << "Area " << areaId
                  << " requested but not configured for this node.";
    // TODO: migration workaround => if me/peer does is using default area,
    // always honor my config, ignore peer's config.
    if (kvStoreDb_.size() == 1 and
        (kvStoreDb_.count(openr::thrift::Types_constants::kDefaultArea()) or
         areaId == openr::thrift::Types_constants::kDefaultArea())) {
      XLOG(INFO) << "Falling back to my single area: "
                 << kvStoreDb_.begin()->first;
      fb303::fbData->addStatValue(
          fmt::format("kvstore.default_area_compatibility.{}", caller),
          1,
          fb303::COUNT);
      return kvStoreDb_.begin()->second;
    } else {
      throw thrift::OpenrError(fmt::format("Invalid area: {}", areaId));
    }
  }
  return search->second;
}

void
KvStore::processCmdSocketRequest(std::vector<fbzmq::Message>&& req) noexcept {
  if (req.empty()) {
    XLOG(ERR) << "Empty request received";
    return;
  }
  auto maybeReply = processRequestMsg(
      req.front().read<std::string>().value(), std::move(req.back()));
  req.pop_back();

  // All messages of the multipart request except the last are sent back as they
  // are ids or empty delims. Add the response at the end of that list.
  if (maybeReply.hasValue()) {
    req.emplace_back(std::move(maybeReply.value()));
  } else {
    req.emplace_back(
        fbzmq::Message::from(Constants::kErrorResponse.toString()).value());
  }

  if (not req.back().empty()) {
    auto sndRet = kvParams_.globalCmdSock.sendMultiple(req);
    if (sndRet.hasError()) {
      XLOG(ERR) << "Error sending response. " << sndRet.error();
    }
  }
  return;
}

void
KvStore::processKeyValueRequest(KeyValueRequest&& kvRequest) {
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
  } catch (thrift::OpenrError const& e) {
    XLOG(ERR) << " Failed to find area " << area.t << " in kvStoreDb_.";
  }
}

folly::Expected<fbzmq::Message, fbzmq::Error>
KvStore::processRequestMsg(
    const std::string& requestId, fbzmq::Message&& request) {
  fb303::fbData->addStatValue(
      "kvstore.peers.bytes_received", request.size(), fb303::SUM);
  auto maybeThriftReq =
      request.readThriftObj<thrift::KvStoreRequest>(serializer_);

  if (maybeThriftReq.hasError()) {
    XLOG(ERR) << "processRequest: failed reading thrift::processRequestMsg"
              << maybeThriftReq.error();
    return {folly::makeUnexpected(fbzmq::Error())};
  }

  auto& thriftRequest = maybeThriftReq.value();
  CHECK(not thriftRequest.area_ref()->empty());

  try {
    auto& kvStoreDb =
        getAreaDbOrThrow(thriftRequest.get_area(), "processRequestMsg");
    XLOG(DBG2) << "Request received for area " << kvStoreDb.getAreaId();
    auto response = kvStoreDb.processRequestMsgHelper(requestId, thriftRequest);
    if (response.hasValue()) {
      fb303::fbData->addStatValue(
          "kvstore.peers.bytes_sent", response->size(), fb303::SUM);
    }
    return response;
  } catch (thrift::OpenrError const& e) {
    return folly::makeUnexpected(fbzmq::Error(0, e.get_message()));
  }
  return {folly::makeUnexpected(fbzmq::Error())};
}

messaging::RQueue<Publication>
KvStore::getKvStoreUpdatesReader() {
  return kvParams_.kvStoreUpdatesQueue.getReader();
}

void
KvStore::processPeerUpdates(PeerEvent&& event) {
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

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStore::semifuture_getKvStoreKeyVals(
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

      auto thriftPub = kvStoreDb.getKeyVals(*keyGetParams.keys_ref());
      kvStoreDb.updatePublicationTtl(thriftPub);

      p.setValue(std::make_unique<thrift::Publication>(std::move(thriftPub)));
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<SelfOriginatedKeyVals>>
KvStore::semifuture_dumpKvStoreSelfOriginatedKeys(std::string area) {
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
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::Publication>>>
KvStore::semifuture_dumpKvStoreKeys(
    thrift::KeyDumpParams keyDumpParams, std::set<std::string> selectAreas) {
  folly::Promise<std::unique_ptr<std::vector<thrift::Publication>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        selectAreas = std::move(selectAreas),
                        keyDumpParams = std::move(keyDumpParams)]() mutable {
    XLOG(DBG3)
        << "Dump all keys requested for "
        << (selectAreas.empty()
                ? "all areas."
                : fmt::format("areas: {}.", folly::join(", ", selectAreas)));

    auto result = std::make_unique<std::vector<thrift::Publication>>();
    for (auto& area : selectAreas) {
      try {
        auto& kvStoreDb = getAreaDbOrThrow(area, "dumpKvStoreKeys");
        fb303::fbData->addStatValue("kvstore.cmd_key_dump", 1, fb303::COUNT);

        std::vector<std::string> keyPrefixList;
        if (keyDumpParams.keys_ref().has_value()) {
          keyPrefixList = *keyDumpParams.keys_ref();
        } else {
          folly::split(",", *keyDumpParams.prefix_ref(), keyPrefixList, true);
        }
        const auto keyPrefixMatch =
            KvStoreFilters(keyPrefixList, *keyDumpParams.originatorIds_ref());

        thrift::FilterOperator oper = thrift::FilterOperator::OR;
        if (keyDumpParams.oper_ref().has_value()) {
          oper = *keyDumpParams.oper_ref();
        }

        auto thriftPub = kvStoreDb.dumpAllWithFilters(
            keyPrefixMatch, oper, *keyDumpParams.doNotPublishValue_ref());
        if (keyDumpParams.keyValHashes_ref().has_value()) {
          thriftPub = kvStoreDb.dumpDifference(
              *thriftPub.keyVals_ref(),
              keyDumpParams.keyValHashes_ref().value());
        }
        kvStoreDb.updatePublicationTtl(thriftPub);
        // I'm the initiator, set flood-root-id
        thriftPub.floodRootId_ref().from_optional(kvStoreDb.getSptRootId());

        if (keyDumpParams.keyValHashes_ref().has_value() and
            (*keyDumpParams.prefix_ref()).empty() and
            (not keyDumpParams.keys_ref().has_value() or
             (*keyDumpParams.keys_ref()).empty())) {
          // This usually comes from neighbor nodes
          size_t numMissingKeys = 0;
          if (thriftPub.tobeUpdatedKeys_ref().has_value()) {
            numMissingKeys = thriftPub.tobeUpdatedKeys_ref()->size();
          }
          XLOG(INFO) << "[Thrift Sync] Processed full-sync request with "
                     << keyDumpParams.keyValHashes_ref().value().size()
                     << " keyValHashes item(s). Sending "
                     << thriftPub.keyVals_ref()->size() << " key-vals and "
                     << numMissingKeys << " missing keys";
        }
        result->push_back(std::move(thriftPub));
      } catch (thrift::OpenrError const& e) {
        XLOG(ERR) << " Failed to find area " << area << " in kvStoreDb_.";
      }
    }
    p.setValue(std::move(result));
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStore::semifuture_dumpKvStoreHashes(
    std::string area, thrift::KeyDumpParams keyDumpParams) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keyDumpParams = std::move(keyDumpParams),
                        area]() mutable {
    XLOG(DBG3) << "Dump all hashes requested for AREA: " << area;
    try {
      auto& kvStoreDb = getAreaDbOrThrow(area, "semifuture_dumpKvStoreHashes");
      fb303::fbData->addStatValue("kvstore.cmd_hash_dump", 1, fb303::COUNT);

      std::set<std::string> originator{};
      std::vector<std::string> keyPrefixList{};
      if (keyDumpParams.keys_ref().has_value()) {
        keyPrefixList = *keyDumpParams.keys_ref();
      } else {
        folly::split(",", *keyDumpParams.prefix_ref(), keyPrefixList, true);
      }
      KvStoreFilters kvFilters{keyPrefixList, originator};
      auto thriftPub = kvStoreDb.dumpHashWithFilters(kvFilters);
      kvStoreDb.updatePublicationTtl(thriftPub);
      p.setValue(std::make_unique<thrift::Publication>(std::move(thriftPub)));
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::semifuture_setKvStoreKeyVals(
    std::string area, thrift::KeySetParams keySetParams) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        keySetParams = std::move(keySetParams),
                        area]() mutable {
    XLOG(DBG3) << "Set key requested for AREA: " << area;
    try {
      auto& kvStoreDb = getAreaDbOrThrow(area, "setKvStoreKeyVals");
      kvStoreDb.setKeyVals(std::move(keySetParams));
      // ready to return
      p.setValue();
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<std::optional<thrift::KvStorePeerState>>
KvStore::semifuture_getKvStorePeerState(
    std::string const& area, std::string const& peerName) {
  folly::Promise<std::optional<thrift::KvStorePeerState>> promise;
  auto sf = promise.getSemiFuture();
  runInEventBaseThread(
      [this, p = std::move(promise), peerName, area]() mutable {
        try {
          p.setValue(getAreaDbOrThrow(area, "semifuture_getKvStorePeerState")
                         .getCurrentState(peerName));
        } catch (thrift::OpenrError const& e) {
          p.setException(e);
        }
      });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
KvStore::semifuture_getKvStorePeers(std::string area) {
  folly::Promise<std::unique_ptr<thrift::PeersMap>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), area]() mutable {
    XLOG(DBG2) << "Peer dump requested for AREA: " << area;
    try {
      p.setValue(std::make_unique<thrift::PeersMap>(
          getAreaDbOrThrow(area, "semifuture_getKvStorePeers").dumpPeers()));
      fb303::fbData->addStatValue("kvstore.cmd_peer_dump", 1, fb303::COUNT);
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>>
KvStore::semifuture_getKvStoreAreaSummaryInternal(
    std::set<std::string> selectAreas) {
  folly::Promise<std::unique_ptr<std::vector<thrift::KvStoreAreaSummary>>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        selectAreas = std::move(selectAreas)]() mutable {
    XLOG(INFO)
        << "KvStore Summary requested for "
        << (selectAreas.empty()
                ? "all areas."
                : fmt::format("areas: {}.", folly::join(", ", selectAreas)));

    auto result = std::make_unique<std::vector<thrift::KvStoreAreaSummary>>();
    for (auto& [area, kvStoreDb] : kvStoreDb_) {
      thrift::KvStoreAreaSummary areaSummary;

      areaSummary.area_ref() = area;
      auto kvDbCounters = kvStoreDb.getCounters();
      areaSummary.keyValsCount_ref() = kvDbCounters["kvstore.num_keys"];
      areaSummary.peersMap_ref() = kvStoreDb.dumpPeers();
      areaSummary.keyValsBytes_ref() = kvStoreDb.getKeyValsSize();

      result->emplace_back(std::move(areaSummary));
    }
    p.setValue(std::move(result));
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::semifuture_addUpdateKvStorePeers(
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
        p.setException(thrift::OpenrError(
            "Empty peerNames from peer-add request, ignoring"));
      } else {
        fb303::fbData->addStatValue("kvstore.cmd_peer_add", 1, fb303::COUNT);
        kvStoreDb.addPeers(peersToAdd);
        p.setValue();
      }
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::semifuture_deleteKvStorePeers(
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
        p.setException(thrift::OpenrError(
            "Empty peerNames from peer-del request, ignoring"));
      } else {
        fb303::fbData->addStatValue("kvstore.cmd_per_del", 1, fb303::COUNT);
        kvStoreDb.delPeers(peersToDel);
        p.setValue();
      }
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<std::unique_ptr<thrift::SptInfos>>
KvStore::semifuture_getSpanningTreeInfos(std::string area) {
  folly::Promise<std::unique_ptr<thrift::SptInfos>> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this, p = std::move(p), area]() mutable {
    XLOG(DBG3) << "FLOOD_TOPO_GET command requested for AREA: " << area;
    try {
      p.setValue(std::make_unique<thrift::SptInfos>(
          getAreaDbOrThrow(area, "semifuture_getSpanningTreeInfos")
              .processFloodTopoGet()));
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::semifuture_updateFloodTopologyChild(
    std::string area, thrift::FloodTopoSetParams floodTopoSetParams) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        floodTopoSetParams = std::move(floodTopoSetParams),
                        area]() mutable {
    XLOG(DBG2) << "FLOOD_TOPO_SET command requested for AREA: " << area;
    try {
      getAreaDbOrThrow(area, "semifuture_updateFloodTopologyChild")
          .processFloodTopoSet(std::move(floodTopoSetParams));
      p.setValue();
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

folly::SemiFuture<folly::Unit>
KvStore::semifuture_processKvStoreDualMessage(
    std::string area, thrift::DualMessages dualMessages) {
  folly::Promise<folly::Unit> p;
  auto sf = p.getSemiFuture();
  runInEventBaseThread([this,
                        p = std::move(p),
                        dualMessages = std::move(dualMessages),
                        area]() mutable {
    XLOG(DBG2) << "DUAL messages received for AREA: " << area;
    try {
      auto& kvStoreDb =
          getAreaDbOrThrow(area, "semifuture_processKvStoreDualMessage");
      if (dualMessages.messages_ref()->empty()) {
        XLOG(ERR) << "Empty DUAL msg receved";
        p.setValue();
      } else {
        fb303::fbData->addStatValue(
            "kvstore.received_dual_messages", 1, fb303::COUNT);

        kvStoreDb.processDualMessages(std::move(dualMessages));
        p.setValue();
      }
    } catch (thrift::OpenrError const& e) {
      p.setException(e);
    }
  });
  return sf;
}

void
KvStore::initialKvStoreDbSynced() {
  for (auto& [_, kvStoreDb] : kvStoreDb_) {
    if (not kvStoreDb.getInitialSyncedWithPeers()) {
      return;
    }
  }

  if (not initialSyncSignalSent_) {
    // Publish KvStore synced signal.
    kvParams_.kvStoreUpdatesQueue.push(Publication(true /*kvStoreSynced*/));
    initialSyncSignalSent_ = true;
    logInitializationEvent(
        "KvStore",
        thrift::InitializationEvent::KVSTORE_SYNCED,
        fmt::format(
            "KvStoreDb sync is completed in all {} areas.", kvStoreDb_.size()));
  }
}

folly::SemiFuture<std::map<std::string, int64_t>>
KvStore::semifuture_getCounters() {
  auto pf = folly::makePromiseContract<std::map<std::string, int64_t>>();
  runInEventBaseThread([this, p = std::move(pf.first)]() mutable {
    p.setValue(getGlobalCounters());
  });
  return std::move(pf.second);
}

std::map<std::string, int64_t>
KvStore::getGlobalCounters() const {
  std::map<std::string, int64_t> flatCounters;
  for (auto& [_, kvDb] : kvStoreDb_) {
    auto kvDbCounters = kvDb.getCounters();
    // add up counters for same key from all kvStoreDb instances
    flatCounters = std::accumulate(
        kvDbCounters.begin(),
        kvDbCounters.end(),
        flatCounters,
        [](std::map<std::string, int64_t>& flatCounters,
           const std::pair<const std::string, int64_t>& kvDbCounter) {
          flatCounters[kvDbCounter.first] += kvDbCounter.second;
          return flatCounters;
        });
  }
  return flatCounters;
}

void
KvStore::initGlobalCounters() {
  // Initialize fb303 counter keys for thrift
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_client_connection_failure", fb303::COUNT);
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
      "kvstore.thrift.num_dual_msg_success", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_dual_msg_failure", fb303::COUNT);

  fb303::fbData->addStatExportType(
      "kvstore.thrift.full_sync_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.flood_pub_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.finalized_sync_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.dual_msg_duration_ms", fb303::AVG);

  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_missing_keys", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_flood_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.thrift.num_keyvals_update", fb303::SUM);

  // TODO: remove `kvstore.zmq.*` counters once ZMQ socket is deprecated
  fb303::fbData->addStatExportType("kvstore.zmq.num_missing_keys", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.zmq.num_keyvals_update", fb303::SUM);

  // Initialize stats keys
  fb303::fbData->addStatExportType("kvstore.cmd_hash_dump", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.cmd_self_originated_key_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_get", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_key_set", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_peer_add", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_peer_dump", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.cmd_per_del", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.expired_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.flood_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType("kvstore.full_sync_duration_ms", fb303::AVG);
  fb303::fbData->addStatExportType("kvstore.looped_publications", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.peers.bytes_received", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.peers.bytes_sent", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.rate_limit_keys", fb303::AVG);
  fb303::fbData->addStatExportType("kvstore.rate_limit_suppress", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.received_dual_messages", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.received_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.received_publications", fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.received_redundant_publications", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.sent_key_vals", fb303::SUM);
  fb303::fbData->addStatExportType("kvstore.sent_publications", fb303::COUNT);
  fb303::fbData->addStatExportType("kvstore.updated_key_vals", fb303::SUM);
}

// static util function to fetch peers by state
std::vector<std::string>
KvStoreDb::getPeersByState(thrift::KvStorePeerState state) {
  std::vector<std::string> res;
  for (auto const& [_, peer] : thriftPeers_) {
    if (peer.peerSpec.get_state() == state) {
      res.emplace_back(peer.nodeName);
    }
  }
  return res;
}

// static util function to log state transition
void
KvStoreDb::logStateTransition(
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
std::optional<thrift::KvStorePeerState>
KvStoreDb::getCurrentState(std::string const& peerName) {
  auto thriftPeerIt = thriftPeers_.find(peerName);
  if (thriftPeerIt != thriftPeers_.end()) {
    return thriftPeerIt->second.peerSpec.get_state();
  }
  return std::nullopt;
}

// static util function for state transition
thrift::KvStorePeerState
KvStoreDb::getNextState(
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
                   */
                  {thrift::KvStorePeerState::SYNCING,
                   std::nullopt,
                   std::nullopt,
                   thrift::KvStorePeerState::IDLE},
                  /*
                   * index 1 - SYNCING
                   * SYNC_RESP_RCVD => INITIALIZED
                   * THRIFT_API_ERROR => IDLE
                   */
                  {std::nullopt,
                   std::nullopt,
                   thrift::KvStorePeerState::INITIALIZED,
                   thrift::KvStorePeerState::IDLE},
                  /*
                   * index 2 - INITIALIZED
                   * SYNC_RESP_RCVD => INITIALIZED
                   * THRIFT_API_ERROR => IDLE
                   */
                  {std::nullopt,
                   std::nullopt,
                   thrift::KvStorePeerState::INITIALIZED,
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
KvStoreDb::KvStorePeer::KvStorePeer(
    const std::string& nodeName,
    const std::string& areaTag,
    const thrift::PeerSpec& ps,
    const ExponentialBackoff<std::chrono::milliseconds>& expBackoff)
    : nodeName(nodeName),
      areaTag(areaTag),
      peerSpec(ps),
      expBackoff(expBackoff) {
  peerSpec.state_ref() = thrift::KvStorePeerState::IDLE;
  CHECK(not this->nodeName.empty());
  CHECK(not this->areaTag.empty());
  CHECK(not this->peerSpec.peerAddr_ref()->empty());
  CHECK(
      this->expBackoff.getInitialBackoff() <= this->expBackoff.getMaxBackoff());
}

bool
KvStoreDb::KvStorePeer::getOrCreateThriftClient(
    OpenrEventBase* evb, std::optional<int> maybeIpTos) {
  // use the existing thrift client if any
  if (client) {
    return true;
  }

  try {
    XLOG(INFO) << fmt::format(
        "{} [Thrift Sync] Creating thrift client with addr: {}, port: {}, peerName: {}",
        areaTag,
        peerSpec.get_peerAddr(),
        peerSpec.get_ctrlPort(),
        nodeName);

    // TODO: migrate to secure thrift connection
    auto thriftClient = getOpenrCtrlPlainTextClient(
        *(evb->getEvb()),
        folly::IPAddress(peerSpec.get_peerAddr()), /* v6LinkLocal */
        peerSpec.get_ctrlPort(), /* port to establish TCP connection */
        Constants::kServiceConnTimeout, /* client connection timeout */
        Constants::kServiceProcTimeout, /* request processing timeout */
        folly::AsyncSocket::anyAddress(), /* bindAddress */
        maybeIpTos /* IP_TOS value for control plane */);
    client = std::move(thriftClient);

    // schedule periodic keepAlive time with 20% jitter variance
    auto period = addJitter<std::chrono::seconds>(
        Constants::kThriftClientKeepAliveInterval, 20.0);
    keepAliveTimer->scheduleTimeout(period);
  } catch (std::exception const& e) {
    XLOG(ERR) << fmt::format(
        "{} [Thrift Sync] Failed creating thrift client with addr: {}, port: {}, peerName: {}. Exception: {}",
        areaTag,
        peerSpec.get_peerAddr(),
        peerSpec.get_ctrlPort(),
        nodeName,
        folly::exceptionStr(e));

    // record telemetry for thrift calls
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_client_connection_failure", 1, fb303::COUNT);

    // clean up state for next round of scanning
    keepAliveTimer->cancelTimeout();
    client.reset();
    expBackoff.reportError(); // apply exponential backoff
    return false;
  }
  return true;
}

//
// KvStoreDb is the class instance that maintains the KV pairs with internal
// map per AREA. KvStoreDb will sync with peers to maintain eventual
// consistency. It supports external message exchanging through Thrift channel.
//
// NOTE Monitoring:
// This module exposes fb303 counters that can be leveraged for monitoring
// KvStoreDb's correctness and performance behevior in production
//
//  kvstore.thrift.num_client_connection_failure: # of client creation
//  failures kvstore.thrift.num_full_sync: # of full-sync performed;
//  kvstore.thrift.num_missing_keys: # of missing keys from syncing with
//  peer; kvstore.thrift.num_full_sync_success: # of successful full-sync
//  performed; kvstore.thrift.num_full_sync_failure: # of failed full-sync
//  performed; kvstore.thrift.full_sync_duration_ms: avg time elapsed for a
//  full-sync req;
//
//  kvstore.thrift.num_flood_pub: # of flooding req issued;
//  kvstore.thrift.num_flood_key_vals: # of keyVals one flooding req
//  contains; kvstore.thrift.num_flood_pub_success: # of successful flooding
//  req performed; kvstore.thrift.num_flood_pub_failure: # of failed
//  flooding req performed; kvstore.thrift.flood_pub_duration_ms: avg time
//  elapsed for a flooding req;
//
//  kvstore.thrift.num_finalized_sync: # of finalized full-sync performed;
//  kvstore.thrift.num_finalized_sync_success: # of successful finalized
//  sync performed; kvstore.thrift.num_finalized_sync_failure: # of failed
//  finalized sync performed; kvstore.thrift.finalized_sync_duration_ms: avg
//  time elapsed for a finalized sync req;
//
KvStoreDb::KvStoreDb(
    OpenrEventBase* evb,
    KvStoreParams& kvParams,
    const std::string& area,
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_CLIENT> peerSyncSock,
    bool isFloodRoot,
    const std::string& nodeId,
    std::function<void()> initialKvStoreSyncedCallback)
    : DualNode(nodeId, isFloodRoot),
      kvParams_(kvParams),
      area_(area),
      areaTag_(fmt::format("[Area {}] ", area)),
      peerSyncSock_(std::move(peerSyncSock)),
      initialKvStoreSyncedCallback_(initialKvStoreSyncedCallback),
      evb_(evb) {
  if (kvParams_.floodRate) {
    floodLimiter_ = std::make_unique<folly::BasicTokenBucket<>>(
        *kvParams_.floodRate->flood_msg_per_sec_ref(),
        *kvParams_.floodRate->flood_msg_burst_size_ref());
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

  XLOG(INFO)
      << AreaTag()
      << fmt::format("Starting kvstore DB instance for node: {}", nodeId);

  //
  // Create flood topo task within its own fiber to periodically dump
  // flooding topology.
  //
  evb_->addFiberTask([this]() mutable noexcept { floodTopoDumpTask(); });

  if (kvParams_.enableFloodOptimization) {
    // [TO BE DEPRECATED]
    // Attach socket callbacks/schedule events
    attachCallbacks();

    // [TO BE DEPRECATED]
    // Perform ZMQ full-sync if there are peers to sync with.
    fullSyncTimer_ = folly::AsyncTimeout::make(
        *evb_->getEvb(), [this]() noexcept { requestFullSyncFromPeers(); });
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
  fb303::fbData->addStatExportType("kvstore.sent_key_vals." + area, fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.sent_publications." + area, fb303::COUNT);
  fb303::fbData->addStatExportType(
      "kvstore.updated_key_vals." + area, fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.received_key_vals." + area, fb303::SUM);
  fb303::fbData->addStatExportType(
      "kvstore.received_publications." + area, fb303::COUNT);
}

void
KvStoreDb::stop() {
  XLOG(INFO) << AreaTag() << "Terminating KvStoreDb.";

  // Send stop signal for internal fibers
  floodTopoStopSignal_.post();

  evb_->getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    // Destroy thrift clients associated with peers, which will
    // fulfill promises with exceptions if any.
    thriftPeers_.clear();
    selfOriginatedKeyTtlTimer_.reset();
    advertiseKeyValsTimer_.reset();
    selfOriginatedTtlUpdatesThrottled_.reset();
    unsetSelfOriginatedKeysThrottled_.reset();
    advertiseSelfOriginatedKeysThrottled_.reset();
    XLOG(INFO) << AreaTag() << "Successfully destroyed thriftPeers and timers";
  });

  // remove ZMQ socket
  if (kvParams_.enableFloodOptimization) {
    evb_->removeSocket(fbzmq::RawZmqSocketPtr{*peerSyncSock_});
  }

  XLOG(INFO) << AreaTag() << "Successfully stopped KvStoreDb.";
}

void
KvStoreDb::floodTopoDumpTask() noexcept {
  XLOG(INFO) << AreaTag() << "Starting flood-topo dump fiber task";

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

  XLOG(INFO) << AreaTag() << "Flood-topo dump fiber task got stopped.";
}

void
KvStoreDb::floodTopoDump() noexcept {
  const auto floodRootId = DualNode::getSptRootId();
  const auto& floodPeers = getFloodPeers(floodRootId);

  XLOG(INFO)
      << AreaTag()
      << fmt::format(
             "[Flood Topo] NodeId: {}, SptRootId: {}, flooding peers: [{}]",
             kvParams_.nodeId,
             floodRootId.has_value() ? floodRootId.value() : "NA",
             folly::join(",", floodPeers));
}

void
KvStoreDb::setSelfOriginatedKey(
    std::string const& key, std::string const& value, uint32_t version) {
  XLOG(DBG3) << AreaTag()
             << fmt::format("{} called for key: {}", __FUNCTION__, key);

  // Create 'thrift::Value' object which will be sent to KvStore
  thrift::Value thriftValue = createThriftValue(
      version,
      nodeId,
      value,
      kvParams_.keyTtl.count(),
      0 /* ttl version */,
      0 /* hash */);
  CHECK(thriftValue.value_ref());

  // Use one version number higher than currently in KvStore if not specified
  if (not version) {
    auto it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      thriftValue.version_ref() = it->second.get_version() + 1;
    } else {
      thriftValue.version_ref() = 1;
    }
  }

  // Store self-originated key-vals in cache
  // ATTN: ttl backoff will be set separately in scheduleTtlUpdates()
  auto selfOriginatedVal = SelfOriginatedValue(thriftValue);
  selfOriginatedKeyVals_[key] = std::move(selfOriginatedVal);

  // Advertise key to KvStore
  std::unordered_map<std::string, thrift::Value> keyVals = {{key, thriftValue}};
  thrift::KeySetParams params;
  params.keyVals_ref() = std::move(keyVals);
  setKeyVals(std::move(params));

  // Add ttl backoff and trigger selfOriginatedKeyTtlTimer_
  scheduleTtlUpdates(key, false /* advertiseImmediately */);
}

void
KvStoreDb::persistSelfOriginatedKey(
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
      createThriftValue(0, nodeId, value, kvParams_.keyTtl.count());
  CHECK(thriftValue.value_ref());

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
      thriftValue.version_ref() = 1;
      shouldAdvertise = true;
    } else {
      // Key is NOT persisted but can be found inside KvStore.
      // This can be keys advertised by our previous incarnation.
      thriftValue = keyIt->second;
      // TTL update pub is never saved in kvstore. Value is not std::nullopt.
      DCHECK(thriftValue.value_ref());
    }
  } else {
    // Key has been persisted before
    thriftValue = selfOriginatedKeyIt->second.value;
    if (*thriftValue.value_ref() == value) {
      // this is a no op, return early and change no state
      return;
    }
  }

  // Override thrift::Value if
  //  1) the SAME key is originated by different node;
  //  2) the peristed value has changed;
  if (*thriftValue.originatorId_ref() != nodeId or
      *thriftValue.value_ref() != value) {
    (*thriftValue.version_ref())++;
    thriftValue.ttlVersion_ref() = 0;
    thriftValue.value_ref() = value;
    thriftValue.originatorId_ref() = nodeId;
    shouldAdvertise = true;
  }

  // Override ttl value to new one.
  // ATTN: When ttl changes but value doesn't, we should advertise ttl
  // immediately so that new ttl is in effect.
  const bool hasTtlChanged =
      (kvParams_.keyTtl.count() != thriftValue.get_ttl()) ? true : false;
  thriftValue.ttl_ref() = kvParams_.keyTtl.count();

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

void
KvStoreDb::advertiseSelfOriginatedKeys() {
  XLOG(DBG3) << "Advertising Self Originated Keys. Num keys to advertise: "
             << keysToAdvertise_.size();

  // advertise pending key for each area
  if (keysToAdvertise_.empty()) {
    return;
  }

  // Build set of keys to advertise
  std::unordered_map<std::string, thrift::Value> keyVals{};
  // Build keys to be cleaned from local storage
  std::vector<std::string> keysToClear;

  std::chrono::milliseconds timeout = Constants::kMaxBackoff;
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
    DCHECK(thriftValue.value_ref());
    keyVals.emplace(key, thriftValue);
    keysToClear.emplace_back(key);
  }

  // Advertise key-vals to KvStore
  thrift::KeySetParams params;
  params.keyVals_ref() = std::move(keyVals);
  setKeyVals(std::move(params));

  // clear out variable used for batching advertisements
  for (auto const& key : keysToClear) {
    keysToAdvertise_.erase(key);
  }

  // Schedule next-timeout for processing/clearing backoffs
  XLOG(DBG2) << "Scheduling timer after " << timeout.count() << "ms.";
  advertiseKeyValsTimer_->scheduleTimeout(timeout);
}

void
KvStoreDb::unsetSelfOriginatedKey(
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
  thriftValue.originatorId_ref() = nodeId;
  (*thriftValue.version_ref())++;
  thriftValue.ttlVersion_ref() = 0;
  thriftValue.value_ref() = value;

  keysToUnset_.emplace(key, std::move(thriftValue));
  // Send updates to KvStore via batch processing.
  unsetSelfOriginatedKeysThrottled_->operator()();
}

void
KvStoreDb::eraseSelfOriginatedKey(std::string const& key) {
  XLOG(DBG3) << AreaTag()
             << fmt::format("{} called for key: {}", __FUNCTION__, key);
  selfOriginatedKeyVals_.erase(key);
  keysToAdvertise_.erase(key);
}

void
KvStoreDb::unsetPendingSelfOriginatedKeys() {
  if (keysToUnset_.empty()) {
    return;
  }

  // Build set of keys to update KvStore
  std::unordered_map<std::string, thrift::Value> keyVals;
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
  params.keyVals_ref() = std::move(keyVals);
  setKeyVals(std::move(params));

  // Empty out keysToUnset_
  for (auto const& key : localKeysToUnset) {
    keysToUnset_.erase(key);
  }
}

void
KvStoreDb::scheduleTtlUpdates(
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

void
KvStoreDb::advertiseTtlUpdates() {
  // Build set of keys to advertise ttl updates
  auto timeout = Constants::kMaxTtlUpdateInterval;

  // all key-vals to advertise ttl updates for
  std::unordered_map<std::string, thrift::Value> keyVals;

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
    (*thriftValue.ttlVersion_ref())++;

    // Create copy of thrift::Value without value field for bandwidth efficiency
    // when advertising
    auto advertiseValue = createThriftValue(
        *thriftValue.version_ref(),
        nodeId,
        std::nullopt, /* empty value */
        *thriftValue.ttl_ref(), /* ttl */
        *thriftValue.ttlVersion_ref()) /* ttl version */;

    // Set in keyVals which will be advertised to the kvStore
    DCHECK(not advertiseValue.value_ref());
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
    params.keyVals_ref() = std::move(keyVals);
    setKeyVals(std::move(params));
  }

  // Schedule next-timeout for processing/clearing backoffs
  XLOG(DBG2)
      << AreaTag()
      << fmt::format("Scheduling ttl timer after {}ms.", timeout.count());

  selfOriginatedKeyTtlTimer_->scheduleTimeout(timeout);
}

void
KvStoreDb::setKeyVals(thrift::KeySetParams&& setParams) {
  // Update statistics
  fb303::fbData->addStatValue("kvstore.cmd_key_set", 1, fb303::COUNT);
  if (setParams.timestamp_ms_ref().has_value()) {
    auto floodMs = getUnixTimeStampMs() - setParams.timestamp_ms_ref().value();
    if (floodMs > 0) {
      fb303::fbData->addStatValue(
          "kvstore.flood_duration_ms", floodMs, fb303::AVG);
    }
  }

  // Update hash for key-values
  for (auto& [_, value] : *setParams.keyVals_ref()) {
    if (value.value_ref().has_value()) {
      value.hash_ref() = generateHash(
          *value.version_ref(), *value.originatorId_ref(), value.value_ref());
    }
  }

  // Create publication and merge it with local KvStore
  thrift::Publication rcvdPublication;
  rcvdPublication.keyVals_ref() = std::move(*setParams.keyVals_ref());
  rcvdPublication.nodeIds_ref().move_from(setParams.nodeIds_ref());
  rcvdPublication.floodRootId_ref().move_from(setParams.floodRootId_ref());
  mergePublication(rcvdPublication);
}

void
KvStoreDb::updateTtlCountdownQueue(const thrift::Publication& publication) {
  for (const auto& [key, value] : *publication.keyVals_ref()) {
    if (*value.ttl_ref() != Constants::kTtlInfinity) {
      TtlCountdownQueueEntry queueEntry;
      queueEntry.expiryTime = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(*value.ttl_ref());
      queueEntry.key = key;
      queueEntry.version = *value.version_ref();
      queueEntry.ttlVersion = *value.ttlVersion_ref();
      queueEntry.originatorId = *value.originatorId_ref();

      if ((ttlCountdownQueue_.empty() or
           (queueEntry.expiryTime <= ttlCountdownQueue_.top().expiryTime)) and
          ttlCountdownTimer_) {
        // Reschedule the shorter timeout
        ttlCountdownTimer_->scheduleTimeout(
            std::chrono::milliseconds(*value.ttl_ref()));
      }

      ttlCountdownQueue_.push(std::move(queueEntry));
    }
  }
}

// loop through all key/vals and count the size of KvStoreDB (per area)
size_t
KvStoreDb::getKeyValsSize() const {
  size_t size = 0;
  if (kvStore_.empty()) {
    return size;
  }
  // calculate total of struct members with fixed size once at the beginning
  size_t fixed_size =
      kvStore_.size() * (sizeof(std::string) + sizeof(thrift::Value));

  // loop through all key/vals and add size of each KV entry
  for (auto const& [key, value] : kvStore_) {
    size += key.size() + value.originatorId_ref()->size() +
        value.value_ref()->size();
  }
  size += fixed_size;

  return size;
}

// build publication out of the requested keys (per request)
// if not keys provided, will return publication with empty keyVals
thrift::Publication
KvStoreDb::getKeyVals(std::vector<std::string> const& keys) {
  thrift::Publication thriftPub;
  thriftPub.area_ref() = area_;

  for (auto const& key : keys) {
    // if requested key if found, respond with version and value
    auto it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      // copy here
      thriftPub.keyVals_ref()[key] = it->second;
    }
  }
  return thriftPub;
}

// dump the entries of my KV store whose keys match filter
thrift::Publication
KvStoreDb::dumpAllWithFilters(
    KvStoreFilters const& kvFilters,
    thrift::FilterOperator oper,
    bool doNotPublishValue) const {
  thrift::Publication thriftPub;
  thriftPub.area_ref() = area_;

  switch (oper) {
  case thrift::FilterOperator::AND:
    for (auto const& [key, val] : kvStore_) {
      if (not kvFilters.keyMatchAll(key, val)) {
        continue;
      }
      if (not doNotPublishValue) {
        thriftPub.keyVals_ref()[key] = val;
      } else {
        thriftPub.keyVals_ref()[key] = createThriftValueWithoutBinaryValue(val);
      }
    }
    break;
  default:
    for (auto const& [key, val] : kvStore_) {
      if (not kvFilters.keyMatch(key, val)) {
        continue;
      }
      if (not doNotPublishValue) {
        thriftPub.keyVals_ref()[key] = val;
      } else {
        thriftPub.keyVals_ref()[key] = createThriftValueWithoutBinaryValue(val);
      }
    }
  }
  return thriftPub;
}

// dump the hashes of my KV store whose keys match the given prefix
// if prefix is the empty string, the full hash store is dumped
thrift::Publication
KvStoreDb::dumpHashWithFilters(KvStoreFilters const& kvFilters) const {
  thrift::Publication thriftPub;
  thriftPub.area_ref() = area_;
  for (auto const& [key, val] : kvStore_) {
    if (not kvFilters.keyMatch(key, val)) {
      continue;
    }
    DCHECK(val.hash_ref().has_value());
    auto& value = thriftPub.keyVals_ref()[key];
    value.version_ref() = *val.version_ref();
    value.originatorId_ref() = *val.originatorId_ref();
    value.hash_ref().copy_from(val.hash_ref());
    value.ttl_ref() = *val.ttl_ref();
    value.ttlVersion_ref() = *val.ttlVersion_ref();
  }
  return thriftPub;
}

// dump the keys on which hashes differ from given keyVals
// thriftPub.keyVals: better keys or keys exist only in MY-KEY-VAL
// thriftPub.tobeUpdatedKeys: better keys or keys exist only in REQ-KEY-VAL
// this way, full-sync initiator knows what keys need to send back to finish
// 3-way full-sync
thrift::Publication
KvStoreDb::dumpDifference(
    std::unordered_map<std::string, thrift::Value> const& myKeyVal,
    std::unordered_map<std::string, thrift::Value> const& reqKeyVal) const {
  thrift::Publication thriftPub;
  thriftPub.area_ref() = area_;

  thriftPub.tobeUpdatedKeys_ref() = std::vector<std::string>{};
  std::unordered_set<std::string> allKeys;
  for (const auto& [key, _] : myKeyVal) {
    allKeys.insert(key);
  }
  for (const auto& [key, _] : reqKeyVal) {
    allKeys.insert(key);
  }

  for (const auto& key : allKeys) {
    const auto& myKv = myKeyVal.find(key);
    const auto& reqKv = reqKeyVal.find(key);
    if (myKv == myKeyVal.end()) {
      // not exist in myKeyVal
      thriftPub.tobeUpdatedKeys_ref()->emplace_back(key);
      continue;
    }
    if (reqKv == reqKeyVal.end()) {
      // not exist in reqKeyVal
      thriftPub.keyVals_ref()->emplace(key, myKv->second);
      continue;
    }
    // common key
    const auto& myVal = myKv->second;
    const auto& reqVal = reqKv->second;
    int rc = compareValues(myVal, reqVal);
    if (rc == 1 or rc == -2) {
      // myVal is better or unknown
      thriftPub.keyVals_ref()->emplace(key, myVal);
    }
    if (rc == -1 or rc == -2) {
      // reqVal is better or unknown
      thriftPub.tobeUpdatedKeys_ref()->emplace_back(key);
    }
  }

  return thriftPub;
}

// This function serves the purpose of periodically scanning peers in
// IDLE state and promote them to SYNCING state. The initial dump will
// happen in async nature to unblock KvStore to process other requests.
void
KvStoreDb::requestThriftPeerSync() {
  // minimal timeout for next run
  auto timeout = std::chrono::milliseconds(Constants::kMaxBackoff);

  // pre-fetch of peers in "SYNCING" state for later calculation
  uint32_t numThriftPeersInSync =
      getPeersByState(thrift::KvStorePeerState::SYNCING).size();

  // Scan over thriftPeers to promote IDLE peers to SYNCING
  for (auto& [peerName, thriftPeer] : thriftPeers_) {
    auto& peerSpec = thriftPeer.peerSpec; // thrift::PeerSpec

    // ignore peers in state other than IDLE
    if (thriftPeer.peerSpec.get_state() != thrift::KvStorePeerState::IDLE) {
      continue;
    }

    // update the global minimum timeout value for next try
    if (not thriftPeer.expBackoff.canTryNow()) {
      timeout =
          std::min(timeout, thriftPeer.expBackoff.getTimeRemainingUntilRetry());
      continue;
    }

    // create thrift client and do backoff if can't go through
    if (not thriftPeer.getOrCreateThriftClient(evb_, kvParams_.maybeIpTos)) {
      timeout =
          std::min(timeout, thriftPeer.expBackoff.getTimeRemainingUntilRetry());
      continue;
    }

    // state transition
    auto oldState = thriftPeer.peerSpec.get_state();
    thriftPeer.peerSpec.state_ref() =
        getNextState(oldState, KvStorePeerEvent::PEER_ADD);
    logStateTransition(peerName, oldState, thriftPeer.peerSpec.get_state());

    // mark peer from IDLE -> SYNCING
    numThriftPeersInSync += 1;

    // build KeyDumpParam
    thrift::KeyDumpParams params;
    if (kvParams_.filters.has_value()) {
      std::string keyPrefix =
          folly::join(",", kvParams_.filters.value().getKeyPrefixes());
      /* prefix is for backward compatibility */
      params.prefix_ref() = keyPrefix;
      if (not keyPrefix.empty()) {
        params.keys_ref() = kvParams_.filters.value().getKeyPrefixes();
      }
      params.originatorIds_ref() =
          kvParams_.filters.value().getOriginatorIdList();
    }
    KvStoreFilters kvFilters(
        std::vector<std::string>{}, /* keyPrefixList */
        std::set<std::string>{} /* originator */);
    params.keyValHashes_ref() =
        std::move(*dumpHashWithFilters(kvFilters).keyVals_ref());

    // record telemetry for initial full-sync
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_full_sync", 1, fb303::COUNT);

    XLOG(INFO) << AreaTag()
               << fmt::format(
                      "[Thrift Sync] Initiating full-sync request for peer: {}",
                      peerName);

    // send request over thrift client and attach callback
    auto startTime = std::chrono::steady_clock::now();
    // TODO: switch to getKvStoreKeyValsFiltered() when all nodes have
    // version with area param
    auto sf = thriftPeer.client->semifuture_getKvStoreKeyValsFilteredArea(
        params, area_);
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
    // wait until kMaxBackoff before sending next round of sync
    if (numThriftPeersInSync > parallelSyncLimitOverThrift_) {
      timeout = Constants::kMaxBackoff;
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
void
KvStoreDb::processThriftSuccess(
    std::string const& peerName,
    thrift::Publication&& pub,
    std::chrono::milliseconds timeDelta) {
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
  if (peer.peerSpec.get_state() == thrift::KvStorePeerState::IDLE) {
    XLOG(WARNING)
        << AreaTag()
        << fmt::format(
               "[Thrift Sync] Ignore response from: {} due to IDLE state.",
               peerName);
    return;
  }

  // ATTN: `peerName` is MANDATORY to fulfill the finialized
  //       full-sync with peers.
  const auto kvUpdateCnt = mergePublication(pub, peerName);
  auto numMissingKeys = 0;
  if (pub.tobeUpdatedKeys_ref().has_value()) {
    numMissingKeys = pub.tobeUpdatedKeys_ref()->size();
  }

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
             pub.keyVals_ref()->size(),
             numMissingKeys)
      << fmt::format(
             "Incurred {} key-value updates. Processing time: {}ms",
             kvUpdateCnt,
             timeDelta.count());

  // State transition
  auto oldState = peer.peerSpec.get_state();
  peer.peerSpec.state_ref() =
      getNextState(oldState, KvStorePeerEvent::SYNC_RESP_RCVD);
  logStateTransition(peerName, oldState, peer.peerSpec.get_state());

  // Notify subscribers of KVSTORE_SYNC event
  kvParams_.kvStoreSyncEventsQueue.push(KvStoreSyncEvent(peerName, area_));

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

void
KvStoreDb::processInitializationEvent() {
  int initialSyncSuccessCnt = 0;
  int initialSyncFailureCnt = 0;
  for (const auto& [peerName, peerStore] : thriftPeers_) {
    if (peerStore.peerSpec.get_state() ==
        thrift::KvStorePeerState::INITIALIZED) {
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
void
KvStoreDb::processThriftFailure(
    std::string const& peerName,
    folly::fbstring const& exceptionStr,
    std::chrono::milliseconds timeDelta) {
  // check if it is valid peer(i.e. peer removed in process of syncing)
  if (not thriftPeers_.count(peerName)) {
    return;
  }

  XLOG(INFO) << AreaTag()
             << fmt::format(
                    "Exception: {}. Processing time: {}ms.",
                    exceptionStr,
                    timeDelta.count());

  // reset client to reconnect later in next batch of thriftSyncTimer_
  // scanning
  auto& peer = thriftPeers_.at(peerName);
  peer.keepAliveTimer->cancelTimeout();
  peer.expBackoff.reportError(); // apply exponential backoff
  peer.client.reset();

  // state transition
  auto oldState = peer.peerSpec.get_state();
  peer.peerSpec.state_ref() =
      getNextState(oldState, KvStorePeerEvent::THRIFT_API_ERROR);
  ++peer.numThriftApiErrors;
  logStateTransition(peerName, oldState, peer.peerSpec.get_state());

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

void
KvStoreDb::addThriftPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  // kvstore external sync over thrift port of knob enabled
  for (auto const& [peerName, newPeerSpec] : peers) {
    auto const& supportFloodOptimization =
        newPeerSpec.get_supportFloodOptimization();
    auto const& peerAddr = newPeerSpec.get_peerAddr();

    // try to connect with peer
    auto peerIter = thriftPeers_.find(peerName);
    if (peerIter != thriftPeers_.end()) {
      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[Peer Update] {} is updated with peerAddr: {}, supportFloodOptimization: {}",
                 peerName,
                 peerAddr,
                 supportFloodOptimization);

      const auto& oldPeerSpec = peerIter->second.peerSpec;
      if (*oldPeerSpec.peerAddr_ref() != *newPeerSpec.peerAddr_ref()) {
        // case1: peerSpec updated(i.e. parallel adjacencies can
        //        potentially have peerSpec updated by LM)
        XLOG(INFO) << AreaTag()
                   << fmt::format(
                          "[Peer Update] peerAddr is updated from: {} to: {}",
                          oldPeerSpec.get_peerAddr(),
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
          peerIter->second.peerSpec.get_state(),
          thrift::KvStorePeerState::IDLE);

      peerIter->second.peerSpec = newPeerSpec; // update peerSpec
      peerIter->second.peerSpec.state_ref() =
          thrift::KvStorePeerState::IDLE; // set IDLE initially
      peerIter->second.keepAliveTimer->cancelTimeout(); // cancel timer
      peerIter->second.client.reset(); // destruct thriftClient
    } else {
      // case 3: found a new peer coming up
      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[Peer Add] {} is added with peerAddr: {}, supportFloodOptimization: {}",
                 peerName,
                 peerAddr,
                 supportFloodOptimization);

      KvStorePeer peer(
          peerName,
          AreaTag(),
          newPeerSpec,
          ExponentialBackoff<std::chrono::milliseconds>(
              Constants::kInitialBackoff, Constants::kMaxBackoff));
      // initialize keepAlive timer to make sure thrift client connection
      // will NOT be closed by thrift server due to inactivity
      const auto name = peerName;
      peer.keepAliveTimer =
          folly::AsyncTimeout::make(*(evb_->getEvb()), [this, name]() noexcept {
            auto period = addJitter(Constants::kThriftClientKeepAliveInterval);
            auto& p = thriftPeers_.at(name);
            CHECK(p.client) << "thrift client is NOT initialized";
            p.client->semifuture_getStatus();
            p.keepAliveTimer->scheduleTimeout(period);
          });
      thriftPeers_.emplace(name, std::move(peer));
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

// TODO: replace addPeers with addThriftPeers call
void
KvStoreDb::addPeers(
    std::unordered_map<std::string, thrift::PeerSpec> const& peers) {
  // thrift peer addition
  addThriftPeers(peers);

  // [TO BE DEPRECATED]
  if (kvParams_.enableFloodOptimization) {
    // ZMQ peer addition
    ++peerAddCounter_;
    std::vector<std::string> dualPeersToAdd;
    for (auto const& [peerName, newPeerSpec] : peers) {
      auto const& newPeerCmdId = fmt::format(
          Constants::kGlobalCmdLocalIdTemplate.toString(),
          peerName,
          peerAddCounter_);
      const auto& supportFloodOptimization =
          newPeerSpec.get_supportFloodOptimization();

      try {
        auto it = peers_.find(peerName);
        bool cmdUrlUpdated{false};
        bool isNewPeer{false};

        // add dual peers for both new-peer or update-peer event
        if (supportFloodOptimization) {
          dualPeersToAdd.emplace_back(peerName);
        }

        if (it != peers_.end()) {
          XLOG(INFO)
              << AreaTag()
              << fmt::format("[ZMQ] Updating existing peer {}", peerName);

          const auto& peerSpec = it->second.first;

          if (*peerSpec.cmdUrl_ref() != *newPeerSpec.cmdUrl_ref()) {
            // case1: peer-spec updated (e.g parallel cases)
            cmdUrlUpdated = true;
            XLOG(INFO) << AreaTag()
                       << fmt::format(
                              "[ZMQ] Disconnecting from {} with id {}",
                              peerSpec.get_cmdUrl(),
                              it->second.second);
            const auto ret = peerSyncSock_.disconnect(
                fbzmq::SocketUrl{*peerSpec.cmdUrl_ref()});
            if (ret.hasError()) {
              XLOG(ERR) << AreaTag()
                        << fmt::format(
                               "[ZMQ] Error Disconnecting to URL {}. Error: {}",
                               peerSpec.get_cmdUrl(),
                               ret.error().errString);
            }
            // Remove any pending expected response for old socket-id
            latestSentPeerSync_.erase(it->second.second);
            it->second.second = newPeerCmdId;
          } else {
            // case2. new peer came up (previsously shut down ungracefully)
            XLOG(WARNING)
                << AreaTag()
                << fmt::format(
                       "[ZMQ] New peer {}. Previously shutdown non-gracefully",
                       peerName);
            isNewPeer = true;
          }
          // Update entry with new data
          it->second.first = newPeerSpec;
        } else {
          // case3. new peer came up
          XLOG(INFO) << AreaTag()
                     << fmt::format("[ZMQ] Adding new peer {}", peerName);
          isNewPeer = true;
          cmdUrlUpdated = true;
          std::tie(it, std::ignore) = peers_.emplace(
              peerName, std::make_pair(newPeerSpec, newPeerCmdId));
        }

        if (cmdUrlUpdated) {
          CHECK(newPeerCmdId == it->second.second);
          XLOG(INFO) << AreaTag()
                     << fmt::format(
                            "[ZMQ] Connecting sync channel to {} with id: {}",
                            newPeerSpec.get_cmdUrl(),
                            newPeerCmdId);
          auto const optStatus = peerSyncSock_.setSockOpt(
              ZMQ_CONNECT_RID, newPeerCmdId.data(), newPeerCmdId.size());
          if (optStatus.hasError()) {
            XLOG(ERR)
                << AreaTag()
                << fmt::format(
                       "[ZMQ] Error setting ZMQ_CONNECT_RID with value {}",
                       newPeerCmdId);
          }
          if (peerSyncSock_.connect(fbzmq::SocketUrl{*newPeerSpec.cmdUrl_ref()})
                  .hasError()) {
            XLOG(ERR) << AreaTag()
                      << fmt::format(
                             "[ZMQ] Error connecting to URL {}",
                             newPeerSpec.get_cmdUrl());
          }
        }

        if (isNewPeer and supportFloodOptimization) {
          // make sure let peer to unset-child for me for all roots first
          // after that, I'll be fed with proper dual-events and I'll be
          // chosing new nexthop if need.
          unsetChildAll(peerName);
        }
      } catch (std::exception const& e) {
        XLOG(ERR) << AreaTag()
                  << fmt::format(
                         "[ZMQ] Error connecting to {}, reason: {}",
                         peerName,
                         folly::exceptionStr(e));
      }
    }
    if (not fullSyncTimer_->isScheduled()) {
      fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
    }

    // process dual events if any
    for (const auto& peer : dualPeersToAdd) {
      XLOG(INFO) << AreaTag() << fmt::format("[Dual] peer up: {}", peer);
      DualNode::peerUp(peer, 1 /* link-cost */); // use hop count as metric
    }
  }
}

// Send message via socket
folly::Expected<size_t, fbzmq::Error>
KvStoreDb::sendMessageToPeer(
    const std::string& peerSocketId, const thrift::KvStoreRequest& request) {
  auto msg = fbzmq::Message::fromThriftObj(request, serializer_).value();
  fb303::fbData->addStatValue(
      "kvstore.peers.bytes_sent", msg.size(), fb303::SUM);
  return peerSyncSock_.sendMultiple(
      fbzmq::Message::from(peerSocketId).value(), fbzmq::Message(), msg);
}

std::map<std::string, int64_t>
KvStoreDb::getCounters() const {
  std::map<std::string, int64_t> counters;

  // Add some more flat counters
  counters["kvstore.num_keys"] = kvStore_.size();
  counters["kvstore.num_peers"] = thriftPeers_.size();
  counters["kvstore.num_zmq_peers"] = peers_.size();
  return counters;
}

void
KvStoreDb::delThriftPeers(std::vector<std::string> const& peers) {
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

    XLOG(INFO)
        << AreaTag()
        << fmt::format(
               "[Peer Delete] {} is detached from peerAddr: {}, supportFloodOptimization: {}",
               peerName,
               peerSpec.get_peerAddr(),
               peerSpec.get_supportFloodOptimization());

    // destroy peer info
    peerIter->second.keepAliveTimer.reset();
    peerIter->second.client.reset();
    thriftPeers_.erase(peerIter);
  }
}

// TODO: replace delPeers with delThriftPeers call
void
KvStoreDb::delPeers(std::vector<std::string> const& peers) {
  // thrift peer deletion
  delThriftPeers(peers);

  // [TO BE DEPRECATED]
  if (kvParams_.enableFloodOptimization) {
    // ZMQ peer deletion
    std::vector<std::string> dualPeersToRemove;
    for (auto const& peerName : peers) {
      // not currently subscribed
      auto it = peers_.find(peerName);
      if (it == peers_.end()) {
        XLOG(ERR)
            << AreaTag()
            << fmt::format(
                   "[ZMQ] Trying to delete non-existing peer {}", peerName);
        continue;
      }

      const auto& peerSpec = it->second.first;
      if (peerSpec.get_supportFloodOptimization()) {
        dualPeersToRemove.emplace_back(peerName);
      }

      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[ZMQ] Detaching from: {}, support-flood-optimization: {}",
                 peerSpec.get_cmdUrl(),
                 peerSpec.get_supportFloodOptimization());
      auto syncRes =
          peerSyncSock_.disconnect(fbzmq::SocketUrl{*peerSpec.cmdUrl_ref()});
      if (syncRes.hasError()) {
        XLOG(ERR) << AreaTag()
                  << fmt::format(
                         "[ZMQ] Failed to detach from {}. Error: {}",
                         peerSpec.get_cmdUrl(),
                         syncRes.error().errString);
      }

      peersToSyncWith_.erase(peerName);
      auto const& peerCmdSocketId = it->second.second;
      if (latestSentPeerSync_.count(peerCmdSocketId)) {
        latestSentPeerSync_.erase(peerCmdSocketId);
      }
      peers_.erase(it);
    }

    // remove dual peers if any
    for (const auto& peer : dualPeersToRemove) {
      XLOG(INFO) << AreaTag() << fmt::format("[Dual] peer down: {}", peer);
      DualNode::peerDown(peer);
    }
  }
}

// Get full KEY_DUMP from peersToSyncWith_
void
KvStoreDb::requestFullSyncFromPeers() {
  // minimal timeout for next run
  auto timeout = std::chrono::milliseconds(Constants::kMaxBackoff);

  // Make requests
  for (auto it = peersToSyncWith_.begin(); it != peersToSyncWith_.end();) {
    auto& peerName = it->first;
    auto& expBackoff = it->second;

    if (not expBackoff.canTryNow()) {
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
      continue;
    }

    // Generate and send router-socket id of peer first. If the kvstore of
    // peer is not connected over the router socket then it will error out
    // exception and we will retry again.
    auto const& peerCmdSocketId = peers_.at(peerName).second;

    // Build request
    thrift::KvStoreRequest dumpRequest;
    thrift::KeyDumpParams params;

    if (kvParams_.filters.has_value()) {
      std::string keyPrefix =
          folly::join(",", kvParams_.filters.value().getKeyPrefixes());
      params.prefix_ref() = keyPrefix;
      params.originatorIds_ref() =
          kvParams_.filters.value().getOriginatorIdList();
    }
    std::set<std::string> originator{};
    std::vector<std::string> keyPrefixList{};
    KvStoreFilters kvFilters{keyPrefixList, originator};
    params.keyValHashes_ref() =
        std::move(*dumpHashWithFilters(kvFilters).keyVals_ref());

    dumpRequest.cmd_ref() = thrift::Command::KEY_DUMP;
    dumpRequest.keyDumpParams_ref() = params;
    dumpRequest.area_ref() = area_;

    XLOG(DBG1) << AreaTag()
               << fmt::format(
                      "[ZMQ] Sending full-sync request to peer {} using id {}",
                      peerName,
                      peerCmdSocketId);
    auto const ret = sendMessageToPeer(peerCmdSocketId, dumpRequest);

    if (ret.hasError()) {
      // this could be pretty common on initial connection setup
      XLOG(ERR)
          << AreaTag()
          << fmt::format(
                 "[ZMQ] Failed to send full-sync request to peer {} ", peerName)
          << fmt::format(
                 " using id {}. Error:{}",
                 peerCmdSocketId,
                 ret.error().errString);
      collectSendFailureStats(ret.error(), peerCmdSocketId);
      expBackoff.reportError(); // Apply exponential backoff
      timeout = std::min(timeout, expBackoff.getTimeRemainingUntilRetry());
      ++it;
    } else {
      latestSentPeerSync_[peerCmdSocketId] = std::chrono::steady_clock::now();

      // Remove the iterator
      it = peersToSyncWith_.erase(it);
    }

    // if pending response is above the limit wait until kMaxBackoff before
    // sending next sync request
    if (latestSentPeerSync_.size() >= parallelSyncLimit_) {
      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[ZMQ] {} full-sync in progress which is above limit: {}. ",
                 latestSentPeerSync_.size(),
                 parallelSyncLimit_)
          << "Will send sync request after max timeout or "
          << "on receipt of syncing response.";
      timeout = Constants::kMaxBackoff;
      break;
    }
  } // for

  // schedule fullSyncTimer if there are pending peers to sync with or
  // if maximum allowed pending sync count is reached. Adding a new peer
  // will not initiate full sync request if it's already scheduled
  if (not peersToSyncWith_.empty() ||
      latestSentPeerSync_.size() >= parallelSyncLimit_) {
    XLOG_IF(INFO, peersToSyncWith_.size())
        << AreaTag()
        << fmt::format(
               "[ZMQ] {} peers still require full-sync.",
               peersToSyncWith_.size());
    XLOG(INFO) << AreaTag()
               << fmt::format(
                      "[ZMQ] Scheduling full-sync after {}ms", timeout.count());

    // schedule next timeout
    fullSyncTimer_->scheduleTimeout(timeout);
  }
}

// dump all peers we are subscribed to
thrift::PeersMap
KvStoreDb::dumpPeers() {
  thrift::PeersMap peers;
  for (auto const& [peerName, thriftPeer] : thriftPeers_) {
    peers.emplace(peerName, thriftPeer.peerSpec);
  }
  return peers;
}

// update TTL with remainng time to expire, TTL version remains
// same so existing keys will not be updated with this TTL
void
KvStoreDb::updatePublicationTtl(
    thrift::Publication& thriftPub, bool removeAboutToExpire) {
  auto timeNow = std::chrono::steady_clock::now();
  for (const auto& qE : ttlCountdownQueue_) {
    // Find key and ensure we are taking time from right entry from queue
    auto kv = thriftPub.keyVals_ref()->find(qE.key);
    if (kv == thriftPub.keyVals_ref()->end() or
        *kv->second.version_ref() != qE.version or
        *kv->second.originatorId_ref() != qE.originatorId or
        *kv->second.ttlVersion_ref() != qE.ttlVersion) {
      continue;
    }

    // Compute timeLeft and do sanity check on it
    auto timeLeft = duration_cast<milliseconds>(qE.expiryTime - timeNow);
    if (timeLeft <= kvParams_.ttlDecr) {
      thriftPub.keyVals_ref()->erase(kv);
      continue;
    }

    // filter key from publication if time left is below ttl threshold
    if (removeAboutToExpire and timeLeft < Constants::kTtlThreshold) {
      thriftPub.keyVals_ref()->erase(kv);
      continue;
    }

    // Set the time-left and decrement it by one so that ttl decrement
    // deterministically whenever it is exchanged between KvStores. This
    // will avoid looping of updates between stores.
    kv->second.ttl_ref() = timeLeft.count() - kvParams_.ttlDecr.count();
  }
}

// process a request
folly::Expected<fbzmq::Message, fbzmq::Error>
KvStoreDb::processRequestMsgHelper(
    const std::string& requestId, thrift::KvStoreRequest& thriftReq) {
  XLOG(DBG3) << AreaTag()
             << fmt::format(
                    "[ZMQ] processRequest: command: {} received.",
                    apache::thrift::TEnumTraits<thrift::Command>::findName(
                        *thriftReq.cmd_ref()));

  std::vector<std::string> keys;
  switch (*thriftReq.cmd_ref()) {
  case thrift::Command::KEY_DUMP: {
    XLOG(DBG3) << "Dump all keys requested";
    if (not thriftReq.keyDumpParams_ref().has_value()) {
      XLOG(ERR) << "received none keyDumpParams";
      return folly::makeUnexpected(fbzmq::Error());
    }

    auto& keyDumpParamsVal = thriftReq.keyDumpParams_ref().value();
    fb303::fbData->addStatValue("kvstore.cmd_key_dump", 1, fb303::COUNT);

    std::vector<std::string> keyPrefixList;
    if (keyDumpParamsVal.keys_ref().has_value()) {
      keyPrefixList = *keyDumpParamsVal.keys_ref();
    } else if (keyDumpParamsVal.prefix_ref().is_set()) {
      folly::split(",", *keyDumpParamsVal.prefix_ref(), keyPrefixList, true);
    }

    const auto keyPrefixMatch =
        KvStoreFilters(keyPrefixList, *keyDumpParamsVal.originatorIds_ref());
    auto thriftPub = dumpAllWithFilters(keyPrefixMatch);
    if (auto keyValHashes = keyDumpParamsVal.keyValHashes_ref()) {
      thriftPub = dumpDifference(*thriftPub.keyVals_ref(), *keyValHashes);
    }
    updatePublicationTtl(thriftPub);
    // I'm the initiator, set flood-root-id
    thriftPub.floodRootId_ref().from_optional(DualNode::getSptRootId());

    if (keyDumpParamsVal.keyValHashes_ref() and
        (not keyDumpParamsVal.prefix_ref().is_set() or
         (*keyDumpParamsVal.prefix_ref()).empty()) and
        (not keyDumpParamsVal.keys_ref().has_value() or
         (*keyDumpParamsVal.keys_ref()).empty())) {
      // This usually comes from neighbor nodes
      size_t numMissingKeys = 0;
      if (thriftPub.tobeUpdatedKeys_ref().has_value()) {
        numMissingKeys = thriftPub.tobeUpdatedKeys_ref()->size();
      }
      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[ZMQ Sync] Processed full-sync request from peer {} with {} keyValHashes item(s). ",
                 requestId,
                 (*keyDumpParamsVal.keyValHashes_ref()).size())
          << fmt::format(
                 "Sending {} key-vals and {} missing keys.",
                 thriftPub.keyVals_ref()->size(),
                 numMissingKeys);
    }
    return fbzmq::Message::fromThriftObj(thriftPub, serializer_);
  }
  case thrift::Command::DUAL: {
    XLOG(DBG2) << "DUAL messages received";
    if (not thriftReq.dualMessages_ref().has_value()) {
      XLOG(ERR) << "received none dualMessages";
      return fbzmq::Message(); // ignore it
    }
    if (thriftReq.dualMessages_ref().value().messages_ref()->empty()) {
      XLOG(WARNING) << AreaTag() << "[ZMQ Sync] received empty dualMessages";
      return fbzmq::Message(); // ignore it
    }
    fb303::fbData->addStatValue(
        "kvstore.received_dual_messages", 1, fb303::COUNT);
    DualNode::processDualMessages(std::move(*thriftReq.dualMessages_ref()));
    return fbzmq::Message();
  }
  case thrift::Command::FLOOD_TOPO_SET: {
    XLOG(DBG2) << "FLOOD_TOPO_SET command requested";
    if (not thriftReq.floodTopoSetParams_ref().has_value()) {
      XLOG(ERR) << AreaTag() << "[ZMQ Sync] received none floodTopoSetParams";
      return fbzmq::Message(); // ignore it
    }
    processFloodTopoSet(std::move(*thriftReq.floodTopoSetParams_ref()));
    return fbzmq::Message();
  }
  default: {
    XLOG(ERR) << AreaTag() << "Unknown command received";
    return folly::makeUnexpected(fbzmq::Error());
  }
  }
}

thrift::SptInfos
KvStoreDb::processFloodTopoGet() noexcept {
  thrift::SptInfos sptInfos;
  const auto& duals = DualNode::getDuals();

  // set spt-infos
  for (const auto& [rootId, dual] : duals) {
    const auto& info = dual.getInfo();
    thrift::SptInfo sptInfo;
    sptInfo.passive_ref() = info.sm.state == DualState::PASSIVE;
    sptInfo.cost_ref() = info.distance;
    // convert from std::optional to std::optional
    std::optional<std::string> nexthop = std::nullopt;
    if (info.nexthop.has_value()) {
      nexthop = info.nexthop.value();
    }
    sptInfo.parent_ref().from_optional(nexthop);
    sptInfo.children_ref() = dual.children();
    sptInfos.infos_ref()->emplace(rootId, sptInfo);
  }

  // set counters
  sptInfos.counters_ref() = DualNode::getCounters();

  // set flood root-id and peers
  sptInfos.floodRootId_ref().from_optional(DualNode::getSptRootId());
  std::optional<std::string> floodRootId{std::nullopt};
  if (sptInfos.floodRootId_ref().has_value()) {
    floodRootId = sptInfos.floodRootId_ref().value();
  }
  sptInfos.floodPeers_ref() = getFloodPeers(floodRootId);
  return sptInfos;
}

void
KvStoreDb::processFloodTopoSet(
    const thrift::FloodTopoSetParams& setParams) noexcept {
  if (setParams.allRoots_ref().has_value() and *setParams.allRoots_ref() and
      not(*setParams.setChild_ref())) {
    // process unset-child for all-roots command
    auto& duals = DualNode::getDuals();
    for (auto& [_, dual] : duals) {
      dual.removeChild(*setParams.srcId_ref());
    }
    return;
  }

  if (not DualNode::hasDual(*setParams.rootId_ref())) {
    XLOG(ERR) << AreaTag()
              << fmt::format(
                     "[Dual] processFloodTopoSet unknown root-id: {}",
                     *setParams.rootId_ref());
    return;
  }
  auto& dual = DualNode::getDual(*setParams.rootId_ref());
  const auto& child = *setParams.srcId_ref();
  if (*setParams.setChild_ref()) {
    // set child command
    XLOG(INFO) << AreaTag()
               << fmt::format(
                      "[Dual] child set: root-id: {}, child: {}",
                      *setParams.rootId_ref(),
                      *setParams.srcId_ref());
    dual.addChild(child);
  } else {
    // unset child command
    XLOG(INFO) << AreaTag()
               << fmt::format(
                      "[Dual] child unset: root-id: {}, child: {}",
                      *setParams.rootId_ref(),
                      *setParams.srcId_ref());
    dual.removeChild(child);
  }
}

void
KvStoreDb::sendTopoSetCmd(
    const std::string& rootId,
    const std::string& peerName,
    bool setChild,
    bool allRoots) noexcept {
  thrift::FloodTopoSetParams setParams;
  setParams.rootId_ref() = rootId;
  setParams.srcId_ref() = kvParams_.nodeId;
  setParams.setChild_ref() = setChild;
  if (allRoots) {
    setParams.allRoots_ref() = allRoots;
  }

  if (kvParams_.enableThriftDualMsg) {
    auto peerIt = thriftPeers_.find(peerName);
    if (peerIt == thriftPeers_.end() or (not peerIt->second.client)) {
      XLOG(ERR) << AreaTag()
                << fmt::format(
                       "[Dual] Invalid dual peer: {} to set topo cmd. Skip.",
                       peerName);
      return;
    }
    auto& client = peerIt->second.client;
    auto startTime = std::chrono::steady_clock::now();
    auto sf = client->semifuture_updateFloodTopologyChild(setParams, area_);
    std::move(sf)
        .via(evb_->getEvb())
        .thenValue([startTime](folly::Unit&&) {
          auto endTime = std::chrono::steady_clock::now();
          auto timeDelta =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  endTime - startTime);

          // record telemetry for thrift calls
          fb303::fbData->addStatValue(
              "kvstore.thrift.num_dual_msg_success", 1, fb303::COUNT);
          fb303::fbData->addStatValue(
              "kvstore.thrift.dual_msg_duration_ms",
              timeDelta.count(),
              fb303::AVG);
        })
        .thenError(
            [this, peerName, startTime](const folly::exception_wrapper& ew) {
              // state transition to IDLE
              auto endTime = std::chrono::steady_clock::now();
              auto timeDelta =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      endTime - startTime);
              processThriftFailure(
                  peerName,
                  fmt::format(
                      "DUAL TOPO_SET failure with {}, {}", peerName, ew.what()),
                  timeDelta);

              // record telemetry for thrift calls
              fb303::fbData->addStatValue(
                  "kvstore.thrift.num_dual_msg_failure", 1, fb303::COUNT);
            });
  } else {
    thrift::KvStoreRequest request;
    request.cmd_ref() = thrift::Command::FLOOD_TOPO_SET;
    request.floodTopoSetParams_ref() = setParams;
    request.area_ref() = area_;

    const auto& dstCmdSocketId = peers_.at(peerName).second;
    const auto ret = sendMessageToPeer(dstCmdSocketId, request);
    if (ret.hasError()) {
      XLOG(ERR) << AreaTag()
                << fmt::format(
                       "{}: failed to {} spt-parent {}. Error: {}",
                       rootId,
                       (setChild ? "set" : "unset"),
                       peerName,
                       ret.error().errString);
      collectSendFailureStats(ret.error(), dstCmdSocketId);
    }
  }
}

void
KvStoreDb::setChild(
    const std::string& rootId, const std::string& peerName) noexcept {
  sendTopoSetCmd(rootId, peerName, true, false);
}

void
KvStoreDb::unsetChild(
    const std::string& rootId, const std::string& peerName) noexcept {
  sendTopoSetCmd(rootId, peerName, false, false);
}

void
KvStoreDb::unsetChildAll(const std::string& peerName) noexcept {
  sendTopoSetCmd("" /* root-id is ignored */, peerName, false, true);
}

void
KvStoreDb::processNexthopChange(
    const std::string& rootId,
    const std::optional<std::string>& oldNh,
    const std::optional<std::string>& newNh) noexcept {
  // sanity check
  std::string oldNhStr = oldNh.has_value() ? *oldNh : "none";
  std::string newNhStr = newNh.has_value() ? *newNh : "none";
  CHECK(oldNh != newNh)
      << rootId
      << ": callback invoked while nexthop does not change: " << oldNhStr;
  // root should NEVER change its nexthop (nexthop always equal to myself)
  CHECK_NE(kvParams_.nodeId, rootId);

  XLOG(INFO) << AreaTag()
             << fmt::format(
                    "[Dual] nexthop change: root-id ({}), {} -> {}",
                    rootId,
                    oldNhStr,
                    newNhStr);

  // set new parent if any
  if (newNh.has_value()) {
    // thriftPeers_ MUST have this new parent
    // if thriftPeers_ does not have this peer, that means KvStore already
    // recevied NEIGHBOR-DOWN event (so does dual), but dual still think I
    // should have this neighbor as nexthop, then something is wrong with
    // DUAL
    if (kvParams_.enableThriftDualMsg) {
      CHECK(thriftPeers_.count(*newNh))
          << rootId << ": trying to set new spt-parent who does not exist "
          << *newNh;
      CHECK_NE(kvParams_.nodeId, *newNh) << "new nexthop is myself";
      setChild(rootId, *newNh);

      // Enqueue new-nexthop for full-sync (insert only if entry doesn't
      // exists) NOTE we have to perform full-sync after we do FLOOD_TOPO_SET,
      // so that we can be sure that I won't be in a disconnected state after
      // we got full synced. (ps: full-sync is 3-way-sync, one direction sync
      // should be good enough)
      //
      // state transition to IDLE to initiate full-sync
      auto& peerSpec = thriftPeers_.at(*newNh).peerSpec;

      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[Dual] Toggle state to idle for peer: {} with dual nexthop change.",
                 *newNh);

      logStateTransition(
          *newNh, peerSpec.get_state(), thrift::KvStorePeerState::IDLE);

      peerSpec.state_ref() =
          thrift::KvStorePeerState::IDLE; // set IDLE to trigger full-sync

      // kick off thriftSyncTimer_ if not yet to asyc process full-sync
      if (not thriftSyncTimer_->isScheduled()) {
        thriftSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
      }
    } else {
      CHECK(peers_.count(*newNh))
          << rootId << ": trying to set new spt-parent who does not exist "
          << *newNh;
      CHECK_NE(kvParams_.nodeId, *newNh) << "new nexthop is myself";
      setChild(rootId, *newNh);

      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "[Dual] Enqueuing full-sync request for peer {}", *newNh);

      peersToSyncWith_.emplace(
          *newNh,
          ExponentialBackoff<std::chrono::milliseconds>(
              Constants::kInitialBackoff, Constants::kMaxBackoff));

      // initial full-sync request if peersToSyncWith_ was empty
      if (not fullSyncTimer_->isScheduled()) {
        fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
      }
    }
  }

  // unset old parent if any
  if (oldNh.has_value()) {
    if ((kvParams_.enableThriftDualMsg and thriftPeers_.count(*oldNh)) or
        ((not kvParams_.enableThriftDualMsg) and peers_.count(*oldNh))) {
      // valid old parent AND it's still my peer, unset it
      CHECK_NE(kvParams_.nodeId, *oldNh) << "old nexthop was myself";
      // unset it
      unsetChild(rootId, *oldNh);
    }
  }
}

void
KvStoreDb::processSyncResponse(
    const std::string& requestId, fbzmq::Message&& syncPubMsg) noexcept {
  fb303::fbData->addStatValue(
      "kvstore.peers.bytes_received", syncPubMsg.size(), fb303::SUM);

  // syncPubMsg can be of two types
  // 1. ack to SET_KEY ("OK" or "ERR")
  // 2. response of KEY_DUMP (thrift::Publication)
  // We check for first one and then fallback to second one
  if (syncPubMsg.size() < 3) {
    auto syncPubStr = syncPubMsg.read<std::string>().value();
    if (syncPubStr == Constants::kErrorResponse) {
      XLOG(ERR) << "Got error for sent publication from " << requestId;
      return;
    }
    if (syncPubStr == Constants::kSuccessResponse) {
      XLOG(DBG2) << "Got ack for sent publication on " << requestId;
      return;
    }
  }

  // Perform error check
  auto maybeSyncPub =
      syncPubMsg.readThriftObj<thrift::Publication>(serializer_);
  if (maybeSyncPub.hasError()) {
    XLOG(ERR) << "Received bad response on peerSyncSock";
    return;
  }

  const auto& syncPub = maybeSyncPub.value();
  const size_t kvUpdateCnt = mergePublication(syncPub, requestId);
  size_t numMissingKeys = 0;

  XLOG(INFO)
      << AreaTag()
      << fmt::format(
             "[ZMQ Sync]: Full-sync response received from {} with {} key-vals. ",
             requestId,
             syncPub.keyVals_ref()->size())
      << fmt::format("Incurred {} key-value updates.", kvUpdateCnt);

  if (syncPub.tobeUpdatedKeys_ref().has_value()) {
    numMissingKeys = syncPub.tobeUpdatedKeys_ref()->size();

    XLOG(INFO) << AreaTag()
               << fmt::format(
                      "[ZMQ Sync]: {} missing keys observed: {}",
                      numMissingKeys,
                      folly::join(",", *syncPub.tobeUpdatedKeys_ref()));
  }

  fb303::fbData->addStatValue(
      "kvstore.zmq.num_missing_keys", numMissingKeys, fb303::SUM);
  fb303::fbData->addStatValue(
      "kvstore.zmq.num_keyvals_update", kvUpdateCnt, fb303::SUM);

  if (latestSentPeerSync_.count(requestId)) {
    auto syncDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - latestSentPeerSync_.at(requestId));
    fb303::fbData->addStatValue(
        "kvstore.full_sync_duration_ms", syncDuration.count(), fb303::AVG);
    XLOG(DBG1) << AreaTag()
               << fmt::format(
                      "It took {}ms to sync with {}.",
                      syncDuration.count(),
                      requestId);
    latestSentPeerSync_.erase(requestId);
  }

  // We've received a full sync response. Double the parallel sync-request
  // limit. This is under assumption that, subsequent sync request will not
  // incur huge changes.
  parallelSyncLimit_ = std::min(
      2 * parallelSyncLimit_, Constants::kMaxFullSyncPendingCountThreshold);

  // Schedule timeout immediately to resume sending full sync requests. If
  // no outstanding sync is required, then cancel the timeout. Cancelling
  // timeout will let the subsequent sync requests to proceed immediately.
  if (not peersToSyncWith_.empty()) {
    fullSyncTimer_->scheduleTimeout(std::chrono::milliseconds(0));
  } else {
    fullSyncTimer_->cancelTimeout();
  }
}

// [TO BE DEPRECATED]
// this will poll the sockets listening to the requests
void
KvStoreDb::attachCallbacks() {
  XLOG(DBG2) << "KvStore: Registering events callbacks ...";

  const auto peersSyncSndHwm = peerSyncSock_.setSockOpt(
      ZMQ_SNDHWM, &kvParams_.zmqHwm, sizeof(kvParams_.zmqHwm));
  if (peersSyncSndHwm.hasError()) {
    XLOG(ERR) << "Error setting ZMQ_SNDHWM to " << kvParams_.zmqHwm << " "
              << peersSyncSndHwm.error();
  }
  const auto peerSyncRcvHwm = peerSyncSock_.setSockOpt(
      ZMQ_RCVHWM, &kvParams_.zmqHwm, sizeof(kvParams_.zmqHwm));
  if (peerSyncRcvHwm.hasError()) {
    XLOG(ERR) << "Error setting ZMQ_SNDHWM to " << kvParams_.zmqHwm << " "
              << peerSyncRcvHwm.error();
  }

  // enable handover for inter process router socket
  const int handover = 1;
  const auto peerSyncHandover =
      peerSyncSock_.setSockOpt(ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (peerSyncHandover.hasError()) {
    XLOG(ERR) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
              << peerSyncHandover.error();
  }

  // set keep-alive to retire old flows
  const auto peerSyncKeepAlive = peerSyncSock_.setKeepAlive(
      Constants::kKeepAliveEnable,
      Constants::kKeepAliveTime.count(),
      Constants::kKeepAliveCnt,
      Constants::kKeepAliveIntvl.count());
  if (peerSyncKeepAlive.hasError()) {
    XLOG(ERR) << "Error setting KeepAlive " << peerSyncKeepAlive.error();
  }

  if (kvParams_.maybeIpTos.has_value()) {
    const int ipTos = kvParams_.maybeIpTos.value();
    const auto peerSyncTos =
        peerSyncSock_.setSockOpt(ZMQ_TOS, &ipTos, sizeof(int));
    if (peerSyncTos.hasError()) {
      XLOG(ERR) << "Error setting ZMQ_TOS to " << ipTos << " "
                << peerSyncTos.error();
    }
  }

  evb_->addSocket(
      fbzmq::RawZmqSocketPtr{*peerSyncSock_}, ZMQ_POLLIN, [this](int) noexcept {
        XLOG(DBG3) << "KvStore: sync response received";
        drainPeerSyncSock();
      });
}

void
KvStoreDb::drainPeerSyncSock() {
  // Drain all available messages in loop
  while (true) {
    fbzmq::Message requestIdMsg, delimMsg, syncPubMsg;
    auto ret = peerSyncSock_.recvMultiple(requestIdMsg, delimMsg, syncPubMsg);
    if (ret.hasError() and ret.error().errNum == EAGAIN) {
      break;
    }

    // Check for error in receiving messages
    if (ret.hasError()) {
      XLOG(ERR) << "failed reading messages from peerSyncSock_: "
                << ret.error();
      continue;
    }

    // at this point we received all three parts
    if (not delimMsg.empty()) {
      XLOG(ERR) << "unexpected delimiter from peerSyncSock_: "
                << delimMsg.read<std::string>().value();
      continue;
    }

    // process the request
    processSyncResponse(
        requestIdMsg.read<std::string>().value(), std::move(syncPubMsg));
  } // while
}

void
KvStoreDb::cleanupTtlCountdownQueue() {
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
    if (it != kvStore_.end() and *it->second.version_ref() == top.version and
        *it->second.originatorId_ref() == top.originatorId and
        *it->second.ttlVersion_ref() == top.ttlVersion) {
      expiredKeys.emplace_back(top.key);
      XLOG(WARNING)
          << AreaTag()
          << "Delete expired (key, version, originatorId, ttlVersion, ttl, node) "
          << fmt::format(
                 "({}, {}, {}, {}, {}, {})",
                 top.key,
                 *it->second.version_ref(),
                 *it->second.originatorId_ref(),
                 *it->second.ttlVersion_ref(),
                 *it->second.ttl_ref(),
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
  expiredKeysPub.expiredKeys_ref() = std::move(expiredKeys);
  expiredKeysPub.area_ref() = area_;
  floodPublication(std::move(expiredKeysPub));
}

void
KvStoreDb::bufferPublication(thrift::Publication&& publication) {
  fb303::fbData->addStatValue("kvstore.rate_limit_suppress", 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.rate_limit_keys", publication.keyVals_ref()->size(), fb303::AVG);
  std::optional<std::string> floodRootId{std::nullopt};
  if (publication.floodRootId_ref().has_value()) {
    floodRootId = publication.floodRootId_ref().value();
  }
  // update or add keys
  for (auto const& [key, _] : *publication.keyVals_ref()) {
    publicationBuffer_[floodRootId].emplace(key);
  }
  for (auto const& key : *publication.expiredKeys_ref()) {
    publicationBuffer_[floodRootId].emplace(key);
  }
}

void
KvStoreDb::floodBufferedUpdates() {
  if (!publicationBuffer_.size()) {
    return;
  }

  // merged-publications to be sent
  std::vector<thrift::Publication> publications;

  // merge publication per root-id
  for (const auto& [rootId, keys] : publicationBuffer_) {
    thrift::Publication publication{};
    // convert from std::optional to std::optional
    std::optional<std::string> floodRootId{std::nullopt};
    if (rootId.has_value()) {
      floodRootId = rootId.value();
    }
    publication.floodRootId_ref().from_optional(floodRootId);
    for (const auto& key : keys) {
      auto kvStoreIt = kvStore_.find(key);
      if (kvStoreIt != kvStore_.end()) {
        publication.keyVals_ref()->emplace(make_pair(key, kvStoreIt->second));
      } else {
        publication.expiredKeys_ref()->emplace_back(key);
      }
    }
    publications.emplace_back(std::move(publication));
  }

  publicationBuffer_.clear();

  for (auto& pub : publications) {
    // when sending out merged publication, we maintain orginal-root-id
    // we act as a forwarder, NOT an initiator. Disable set-flood-root here
    floodPublication(
        std::move(pub), false /* rate-limit */, false /* set-flood-root */);
  }
}

void
KvStoreDb::finalizeFullSync(
    const std::unordered_set<std::string>& keys, const std::string& senderId) {
  // build keyval to be sent
  thrift::Publication updates;
  for (const auto& key : keys) {
    const auto& it = kvStore_.find(key);
    if (it != kvStore_.end()) {
      updates.keyVals_ref()->emplace(key, it->second);
    }
  }

  // Update ttl values to remove expiring keys. Ignore the response if no
  // keys to be sent
  updatePublicationTtl(updates);
  if (not updates.keyVals_ref()->size()) {
    return;
  }

  // Build params for final sync of 3-way handshake
  thrift::KeySetParams params;
  params.keyVals_ref() = std::move(*updates.keyVals_ref());
  // I'm the initiator, set flood-root-id
  params.floodRootId_ref().from_optional(DualNode::getSptRootId());
  params.timestamp_ms_ref() = getUnixTimeStampMs();
  params.set_nodeIds({kvParams_.nodeId});

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
  if (thriftPeer.peerSpec.get_state() == thrift::KvStorePeerState::IDLE or
      (not thriftPeer.client)) {
    // peer in thriftPeers collection can still be in IDLE state.
    // Skip final full-sync with those peers.
    return;
  }

  XLOG(INFO)
      << AreaTag()
      << fmt::format(
             "[Thrift Sync] Finalize full-sync back to: {} with keys: {}",
             senderId,
             folly::join(",", keys));

  // record telemetry for thrift calls
  fb303::fbData->addStatValue(
      "kvstore.thrift.num_finalized_sync", 1, fb303::COUNT);

  auto startTime = std::chrono::steady_clock::now();
  auto sf = thriftPeer.client->semifuture_setKvStoreKeyVals(params, area_);
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

std::unordered_set<std::string>
KvStoreDb::getFloodPeers(const std::optional<std::string>& rootId) {
  auto sptPeers = DualNode::getSptPeers(rootId);
  bool floodToAll = false;
  if (not kvParams_.enableFloodOptimization or sptPeers.empty()) {
    // fall back to naive flooding if feature not enabled or can not find
    // valid SPT-peers
    floodToAll = true;
  }

  // flood-peers:
  //  1) SPT-peers;
  //  2) peers-who-does-not-support-DUAL;
  std::unordered_set<std::string> floodPeers;
  for (const auto& [peerName, peer] : thriftPeers_) {
    if (floodToAll or sptPeers.count(peerName) != 0 or
        (not peer.peerSpec.get_supportFloodOptimization())) {
      floodPeers.emplace(peerName);
    }
  }
  return floodPeers;
}

void
KvStoreDb::collectSendFailureStats(
    const fbzmq::Error& error, const std::string& dstSockId) {
  fb303::fbData->addStatValue(
      fmt::format("kvstore.send_failure.{}.{}", dstSockId, error.errNum),
      1,
      fb303::COUNT);
}

void
KvStoreDb::floodPublication(
    thrift::Publication&& publication, bool rateLimit, bool setFloodRoot) {
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
  updatePublicationTtl(publication, true);

  // If there are no changes then return
  if (publication.keyVals_ref()->empty() &&
      publication.expiredKeys_ref()->empty()) {
    return;
  }

  // Find from whom we might have got this publication. Last entry is our ID
  // and hence second last entry is the node from whom we get this
  // publication
  std::optional<std::string> senderId;
  if (publication.nodeIds_ref().has_value() and
      publication.nodeIds_ref()->size()) {
    senderId = publication.nodeIds_ref()->back();
  }
  if (not publication.nodeIds_ref().has_value()) {
    publication.nodeIds_ref() = std::vector<std::string>{};
  }
  publication.nodeIds_ref()->emplace_back(kvParams_.nodeId);

  // Flood publication to internal subscribers
  kvParams_.kvStoreUpdatesQueue.push(Publication(publication));
  fb303::fbData->addStatValue("kvstore.num_updates", 1, fb303::COUNT);

  // Process potential update to self-originated key-vals
  if (kvParams_.enableKvStoreRequestQueue) {
    processPublicationForSelfOriginatedKey(publication);
  }

  // Flood keyValue ONLY updates to external neighbors
  if (publication.keyVals_ref()->empty()) {
    return;
  }

  // Key collection to be flooded
  auto keysToUpdate = folly::gen::from(*publication.keyVals_ref()) |
      folly::gen::get<0>() | folly::gen::as<std::vector<std::string>>();

  XLOG(DBG2) << AreaTag()
             << fmt::format(
                    "Flood publication from: {} to peers with: {} key-vals. ",
                    kvParams_.nodeId,
                    keysToUpdate.size())
             << fmt::format("Updated keys: {}", folly::join(",", keysToUpdate));

  if (setFloodRoot and not senderId.has_value()) {
    // I'm the initiator, set flood-root-id
    publication.floodRootId_ref().from_optional(DualNode::getSptRootId());
  }

  // prepare thrift structure for flooding purpose
  thrift::KeySetParams params;
  params.keyVals_ref() = *publication.keyVals_ref();
  params.nodeIds_ref().copy_from(publication.nodeIds_ref());
  params.floodRootId_ref().copy_from(publication.floodRootId_ref());
  params.timestamp_ms_ref() = getUnixTimeStampMs();

  std::optional<std::string> floodRootId{std::nullopt};
  if (params.floodRootId_ref().has_value()) {
    floodRootId = params.floodRootId_ref().value();
  }
  const auto& floodPeers = getFloodPeers(floodRootId);

  for (const auto& peerName : floodPeers) {
    auto peerIt = thriftPeers_.find(peerName);
    if (peerIt == thriftPeers_.end()) {
      XLOG(ERR) << AreaTag()
                << fmt::format("Invalid flooding peer: {}. Skip it.", peerName);
      continue;
    }

    if (senderId.has_value() && senderId.value() == peerName) {
      // Do not flood towards senderId from whom we received this
      // publication
      continue;
    }

    auto& thriftPeer = peerIt->second;
    if (thriftPeer.peerSpec.get_state() !=
            thrift::KvStorePeerState::INITIALIZED or
        (not thriftPeer.client)) {
      // Skip flooding to those peers if peer has NOT finished
      // initial sync(i.e. promoted to `INITIALIZED`)
      // store key for flooding after intialized
      for (auto const& [key, _] : params.get_keyVals()) {
        thriftPeer.pendingKeysDuringInitialization.insert(key);
      }
      continue;
    }

    // record telemetry for flooding publications
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_flood_pub", 1, fb303::COUNT);
    fb303::fbData->addStatValue(
        "kvstore.thrift.num_flood_key_vals",
        publication.keyVals_ref()->size(),
        fb303::SUM);

    auto startTime = std::chrono::steady_clock::now();
    auto sf = thriftPeer.client->semifuture_setKvStoreKeyVals(params, area_);
    std::move(sf)
        .via(evb_->getEvb())
        .thenValue([peerName, startTime](folly::Unit&&) {
          XLOG(DBG4) << "Flooding ack received from peer: " << peerName;

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
        .thenError([this, peerName, startTime](
                       const folly::exception_wrapper& ew) {
          // state transition to IDLE
          auto endTime = std::chrono::steady_clock::now();
          auto timeDelta =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  endTime - startTime);
          processThriftFailure(
              peerName,
              fmt::format("FLOOD_PUB failure with {}, {}", peerName, ew.what()),
              timeDelta);

          // record telemetry for thrift calls
          fb303::fbData->addStatValue(
              "kvstore.thrift.num_flood_pub_failure", 1, fb303::COUNT);
        });
  }
}

void
KvStoreDb::processPublicationForSelfOriginatedKey(
    thrift::Publication const& publication) {
  // direct return to avoid performance issue
  if (selfOriginatedKeyVals_.empty()) {
    return;
  }

  // go through received publications to refresh self-originated key-vals if
  // necessary
  for (auto const& [key, rcvdValue] : publication.get_keyVals()) {
    if (not rcvdValue.value_ref().has_value()) {
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
    const auto& currVersion = currValue.get_version();
    const auto& rcvdVersion = rcvdValue.get_version();
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
      if (*rcvdValue.originatorId_ref() != kvParams_.nodeId or
          *currValue.value_ref() != *rcvdValue.value_ref()) {
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
      currValue.ttlVersion_ref() = 0;
      currValue.version_ref() = rcvdValue.get_version() + 1;
      keysToAdvertise_.insert(key);

      XLOG(INFO)
          << AreaTag()
          << fmt::format(
                 "Override version for [key: {}, v: {}, originatorId: {}]",
                 key,
                 rcvdValue.get_version(),
                 currValue.get_originatorId());
    } else {
      // update local ttlVersion if received higher ttlVersion.
      // NOTE: ttlVersion will be bumped up before ttl update.
      // It works fine to just update to latest ttlVersion, instead of +1.
      if (currValue.get_ttlVersion() < rcvdValue.get_ttlVersion()) {
        currValue.ttlVersion_ref() = rcvdValue.get_ttlVersion();
      }
    }
  }

  // NOTE: use throttling to NOT block publication flooding.
  advertiseSelfOriginatedKeysThrottled_->operator()();

  // TODO: when native key subscription is supported. Handle callback here.
}

size_t
KvStoreDb::mergePublication(
    const thrift::Publication& rcvdPublication,
    std::optional<std::string> senderId) {
  // Add counters
  fb303::fbData->addStatValue("kvstore.received_publications", 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.received_publications." + area_, 1, fb303::COUNT);
  fb303::fbData->addStatValue(
      "kvstore.received_key_vals",
      rcvdPublication.keyVals_ref()->size(),
      fb303::SUM);
  fb303::fbData->addStatValue(
      "kvstore.received_key_vals." + area_,
      rcvdPublication.keyVals_ref()->size(),
      fb303::SUM);

  static const std::vector<std::string> kUpdatedKeys = {};

  std::unordered_set<std::string> keysTobeUpdated;
  for (auto const& key : rcvdPublication.tobeUpdatedKeys_ref()
           ? rcvdPublication.tobeUpdatedKeys_ref().value()
           : kUpdatedKeys) {
    keysTobeUpdated.insert(key);
  }
  if (senderId.has_value()) {
    auto peerIt = thriftPeers_.find(senderId.value());
    if (peerIt != thriftPeers_.end()) {
      keysTobeUpdated.merge(peerIt->second.pendingKeysDuringInitialization);
      peerIt->second.pendingKeysDuringInitialization.clear();
    }
  }
  const bool needFinalizeFullSync =
      senderId.has_value() and not keysTobeUpdated.empty();

  // This can happen when KvStore is emitting expired-key updates
  if (rcvdPublication.keyVals_ref()->empty() and not needFinalizeFullSync) {
    return 0;
  }

  // Check for loop
  const auto nodeIds = rcvdPublication.nodeIds_ref();
  if (nodeIds.has_value() and
      std::find(nodeIds->cbegin(), nodeIds->cend(), kvParams_.nodeId) !=
          nodeIds->cend()) {
    fb303::fbData->addStatValue("kvstore.looped_publications", 1, fb303::COUNT);
    return 0;
  }

  // Generate delta with local KvStore
  thrift::Publication deltaPublication;
  deltaPublication.keyVals_ref() = mergeKeyValues(
      kvStore_, *rcvdPublication.keyVals_ref(), kvParams_.filters);
  deltaPublication.floodRootId_ref().copy_from(
      rcvdPublication.floodRootId_ref());
  deltaPublication.area_ref() = area_;

  const size_t kvUpdateCnt = deltaPublication.keyVals_ref()->size();
  fb303::fbData->addStatValue(
      "kvstore.updated_key_vals", kvUpdateCnt, fb303::SUM);
  fb303::fbData->addStatValue(
      "kvstore.updated_key_vals." + area_, kvUpdateCnt, fb303::SUM);

  // Populate nodeIds and our nodeId_ to the end
  if (rcvdPublication.nodeIds_ref().has_value()) {
    deltaPublication.nodeIds_ref().copy_from(rcvdPublication.nodeIds_ref());
  }

  // Update ttl values of keys
  updateTtlCountdownQueue(deltaPublication);

  if (not deltaPublication.keyVals_ref()->empty()) {
    // Flood change to all of our neighbors/subscribers
    floodPublication(std::move(deltaPublication));
  } else {
    // Keep track of received publications which din't update any field
    fb303::fbData->addStatValue(
        "kvstore.received_redundant_publications", 1, fb303::COUNT);
  }

  // response to senderId with tobeUpdatedKeys + Vals
  // (last step in 3-way full-sync)
  if (needFinalizeFullSync) {
    finalizeFullSync(keysTobeUpdated, *senderId);
  }

  return kvUpdateCnt;
}

void
KvStoreDb::logSyncEvent(
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

void
KvStoreDb::logKvEvent(const std::string& event, const std::string& key) {
  LogSample sample{};

  sample.addString("area", AreaTag());
  sample.addString("event", event);
  sample.addString("node_name", kvParams_.nodeId);
  sample.addString("key", key);

  kvParams_.logSampleQueue.push(std::move(sample));
}

bool
KvStoreDb::sendDualMessages(
    const std::string& neighbor, const thrift::DualMessages& msgs) noexcept {
  if (kvParams_.enableThriftDualMsg) {
    auto peerIt = thriftPeers_.find(neighbor);
    if (peerIt == thriftPeers_.end() or (not peerIt->second.client)) {
      XLOG(ERR) << AreaTag()
                << fmt::format(
                       "[Dual] Invalid dual peer: {} to set topo cmd. Skip it.",
                       neighbor);
      return false;
    }

    auto& client = peerIt->second.client;
    auto startTime = std::chrono::steady_clock::now();
    auto sf = client->semifuture_processKvStoreDualMessage(msgs, area_);
    std::move(sf)
        .via(evb_->getEvb())
        .thenValue([startTime](folly::Unit&&) {
          auto endTime = std::chrono::steady_clock::now();
          auto timeDelta =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  endTime - startTime);

          // record telemetry for thrift calls
          fb303::fbData->addStatValue(
              "kvstore.thrift.num_dual_msg_success", 1, fb303::COUNT);
          fb303::fbData->addStatValue(
              "kvstore.thrift.dual_msg_duration_ms",
              timeDelta.count(),
              fb303::AVG);
        })
        .thenError([this, neighbor, startTime](
                       const folly::exception_wrapper& ew) {
          // state transition to IDLE
          auto endTime = std::chrono::steady_clock::now();
          auto timeDelta =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  endTime - startTime);
          processThriftFailure(
              neighbor,
              fmt::format("DUAL MSG failure with {}, {}", neighbor, ew.what()),
              timeDelta);

          // record telemetry for thrift calls
          fb303::fbData->addStatValue(
              "kvstore.thrift.num_dual_msg_failure", 1, fb303::COUNT);
        });
  } else {
    if (peers_.count(neighbor) == 0) {
      XLOG(ERR) << AreaTag()
                << fmt::format(
                       "[Dual] Invalid dual peer: {} to set topo cmd. Skip it.",
                       neighbor);
      return false;
    }

    const auto& neighborCmdSocketId = peers_.at(neighbor).second;
    thrift::KvStoreRequest dualRequest;
    dualRequest.cmd_ref() = thrift::Command::DUAL;
    dualRequest.dualMessages_ref() = msgs;
    dualRequest.area_ref() = area_;
    const auto ret = sendMessageToPeer(neighborCmdSocketId, dualRequest);
    // NOTE: we rely on zmq (on top of tcp) to reliably deliver message,
    // if we switch to other protocols, we need to make sure its reliability.
    // Due to zmq async fashion, in case of failure (means the other side
    // is going down), it's ok to lose this pending message since later on,
    // neighor will inform us it's gone. and we will delete it from our dual
    // peers.
    if (ret.hasError()) {
      XLOG(ERR)
          << AreaTag()
          << fmt::format(
                 "[Dual] Failed to send dual messages to {}  using id: {}. ",
                 neighbor,
                 neighborCmdSocketId)
          << fmt::format("Error: {}", ret.error().errString);
      collectSendFailureStats(ret.error(), neighborCmdSocketId);
      return false;
    }
  }
  return true;
}

} // namespace openr
