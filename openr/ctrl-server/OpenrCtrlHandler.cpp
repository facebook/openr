/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/ctrl-server/OpenrCtrlHandler.h>

#include <re2/re2.h>

#include <folly/ExceptionString.h>
#include <folly/io/async/SSLContext.h>
#include <folly/io/async/ssl/OpenSSLUtils.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/PersistentStore_types.h>
#include <openr/if/gen-cpp2/PrefixManager_types.h>

namespace openr {

OpenrCtrlHandler::OpenrCtrlHandler(
    const std::string& nodeName,
    const std::unordered_set<std::string>& acceptablePeerCommonNames,
    std::unordered_map<
        thrift::OpenrModuleType,
        std::shared_ptr<OpenrEventLoop>>& moduleTypeToEvl,
    MonitorSubmitUrl const& monitorSubmitUrl,
    KvStoreLocalPubUrl const& kvStoreLocalPubUrl,
    fbzmq::ZmqEventLoop& evl,
    fbzmq::Context& context)
    : facebook::fb303::FacebookBase2("openr"),
      nodeName_(nodeName),
      acceptablePeerCommonNames_(acceptablePeerCommonNames),
      moduleTypeToEvl_(moduleTypeToEvl),
      evl_(evl),
      kvStoreSubSock_(context) {
  // Create monitor client
  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(context, monitorSubmitUrl);

  // Connect to KvStore
  const auto kvStoreSub =
      kvStoreSubSock_.connect(fbzmq::SocketUrl{kvStoreLocalPubUrl});
  if (kvStoreSub.hasError()) {
    LOG(FATAL) << "Error binding to URL " << std::string(kvStoreLocalPubUrl)
               << " " << kvStoreSub.error();
  }

  // Subscribe to everything
  const auto kvStoreSubOpt = kvStoreSubSock_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (kvStoreSubOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << std::string(kvStoreLocalPubUrl) << " "
               << kvStoreSubOpt.error();
  }

  evl_.runInEventLoop([this]() noexcept {
    evl_.addSocket(
        fbzmq::RawZmqSocketPtr{*kvStoreSubSock_},
        ZMQ_POLLIN,
        [&](int) noexcept {
          apache::thrift::CompactSerializer serializer;
          // Read publication from socket and process it
          auto maybePublication =
              kvStoreSubSock_.recvThriftObj<thrift::Publication>(serializer);
          if (maybePublication.hasError()) {
            LOG(ERROR) << "Failed to read publication from KvStore SUB socket. "
                       << "Exception: " << maybePublication.error();
            return;
          }

          SYNCHRONIZED(kvStorePublishers_) {
            for (auto& kv : kvStorePublishers_) {
              kv.second.next(maybePublication.value());
            }
          }

          bool isAdjChanged = false;
          // check if any of KeyVal has 'adj' update
          for (auto& kv : maybePublication.value().keyVals) {
            auto& key = kv.first;
            auto& val = kv.second;
            // check if we have any value update.
            // Ttl refreshing won't update any value.
            if (!val.value.hasValue()) {
              continue;
            }

            // "adj:*" key has changed. Update local collection
            if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
              VLOG(3) << "Adj key: " << key << " change received";
              isAdjChanged = true;
              break;
            }
          }

          if (isAdjChanged) {
            // thrift::Publication contains "adj:*" key change.
            // Clean ALL pending promises
            longPollReqs_.withWLock([&](auto& longPollReqs) {
              for (auto& kv : longPollReqs) {
                auto& p = kv.second.first;
                p.setValue(true);
              }
              longPollReqs.clear();
            });
          } else {
            longPollReqs_.withWLock([&](auto& longPollReqs) {
              auto now = getUnixTimeStampMs();
              std::vector<int64_t> reqsToClean;
              for (auto& kv : longPollReqs) {
                auto& clientId = kv.first;
                auto& req = kv.second;

                auto& p = req.first;
                auto& timeStamp = req.second;
                if (now - timeStamp >=
                    Constants::kLongPollReqHoldTime.count()) {
                  LOG(INFO) << "Elapsed time: " << now - timeStamp
                            << " is over hold limit: "
                            << Constants::kLongPollReqHoldTime.count();
                  reqsToClean.emplace_back(clientId);
                  p.setException(thrift::OpenrError("Request timed out"));
                }
              }

              // cleanup expired requests since no ADJ change observed
              for (auto& clientId : reqsToClean) {
                longPollReqs.erase(clientId);
              }
            });
          }
        });
  });

  for (const auto& kv : moduleTypeToEvl_) {
    auto moduleType = kv.first;
    auto& inprocUrl = kv.second->inprocCmdUrl;
    auto result = moduleSockets_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(moduleType),
        std::forward_as_tuple(
            context, folly::none, folly::none, fbzmq::NonblockingFlag{false}));
    auto& sock = result.first->second;
    int enabled = 1;
    // if we do not get a reply within the timeout, we reset the state
    sock.setSockOpt(ZMQ_REQ_RELAXED, &enabled, sizeof(int));
    sock.setSockOpt(ZMQ_REQ_CORRELATE, &enabled, sizeof(int));

    const auto rc = sock.connect(fbzmq::SocketUrl{inprocUrl});
    if (rc.hasError()) {
      LOG(FATAL) << "Error connecting to URL '" << kv.second << "' "
                 << rc.error();
    }
  }
}

OpenrCtrlHandler::~OpenrCtrlHandler() {
  evl_.removeSocket(fbzmq::RawZmqSocketPtr{*kvStoreSubSock_});
  kvStoreSubSock_.close();

  std::vector<apache::thrift::StreamPublisher<thrift::Publication>> publishers;
  // NOTE: We're intentionally creating list of publishers to and then invoke
  // `complete()` on them.
  // Reason => `complete()` returns only when callback `onComplete` associated
  // with publisher returns. Since we acquire lock within `onComplete` callback,
  // we will run into the deadlock if `complete()` is invoked within
  // SYNCHRONIZED block
  SYNCHRONIZED(kvStorePublishers_) {
    for (auto& kv : kvStorePublishers_) {
      publishers.emplace_back(std::move(kv.second));
    }
  }
  LOG(INFO) << "Terminating " << publishers.size()
            << " active KvStore snoop stream(s).";
  for (auto& publisher : publishers) {
    std::move(publisher).complete();
  }

  LOG(INFO) << "Cleanup all pending request(s).";
  longPollReqs_.withWLock([&](auto& longPollReqs) { longPollReqs.clear(); });
}

void
OpenrCtrlHandler::authorizeConnection() {
  auto connContext = getConnectionContext()->getConnectionContext();
  auto peerCommonName = connContext->getPeerCommonName();
  auto peerAddr = connContext->getPeerAddress();

  // We legitely accepts all connections (secure/non-secure) from localhost
  if (peerAddr->isLoopbackAddress()) {
    return;
  }

  if (peerCommonName.empty() || acceptablePeerCommonNames_.empty()) {
    // for now, we will allow non-secure connections, but lets log the event so
    // we know how often this is happening.
    fbzmq::LogSample sample{};

    sample.addString(
        "event",
        peerCommonName.empty() ? "UNENCRYPTED_CTRL_CONNECTION"
                               : "UNRESTRICTED_AUTHORIZATION");
    sample.addString("node_name", nodeName_);
    sample.addString(
        "peer_address", connContext->getPeerAddress()->getAddressStr());
    sample.addString("peer_common_name", peerCommonName);

    zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
        apache::thrift::FRAGILE,
        Constants::kEventLogCategory.toString(),
        {sample.toJson()}));

    LOG(INFO) << "Authorizing request with issues: " << sample.toJson();
    return;
  }

  if (!acceptablePeerCommonNames_.count(peerCommonName)) {
    throw thrift::OpenrError(
        folly::sformat("Peer name {} is unacceptable", peerCommonName));
  }
}

folly::Expected<fbzmq::Message, fbzmq::Error>
OpenrCtrlHandler::requestReplyMessage(
    thrift::OpenrModuleType module, fbzmq::Message&& request, bool oneway) {
  // Check module
  auto moduleIt = moduleSockets_.find(module);
  if (moduleIt == moduleSockets_.end()) {
    return folly::makeUnexpected(fbzmq::Error(
        0, folly::sformat("Unknown module {}", static_cast<int>(module))));
  }

  // Send request
  auto& sock = moduleIt->second;
  auto sendRet = sock.sendOne(std::move(request));
  if (sendRet.hasError()) {
    return folly::makeUnexpected(sendRet.error());
  }

  // Recv response if not oneway
  if (oneway) {
    return fbzmq::Message();
  } else {
    return sock.recvOne(Constants::kReadTimeout);
  }
}

template <typename ReturnType, typename InputType>
folly::Expected<ReturnType, fbzmq::Error>
OpenrCtrlHandler::requestReplyThrift(
    thrift::OpenrModuleType module, InputType&& input) {
  apache::thrift::CompactSerializer serializer;
  auto reply = requestReplyMessage(
      module,
      fbzmq::Message::fromThriftObj(std::forward<InputType>(input), serializer)
          .value(),
      false);
  if (reply.hasError()) {
    return folly::makeUnexpected(reply.error());
  }

  // Recv response
  return reply->template readThriftObj<ReturnType>(serializer);
}

template <typename InputType>
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::processThriftRequest(
    thrift::OpenrModuleType module, InputType&& input, bool oneway) {
  folly::Promise<folly::Unit> p;

  apache::thrift::CompactSerializer serializer;
  auto reply = requestReplyMessage(
      module,
      fbzmq::Message::fromThriftObj(std::forward<InputType>(input), serializer)
          .value(),
      oneway);
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue();
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<std::string>>
OpenrCtrlHandler::semifuture_command(
    thrift::OpenrModuleType module, std::unique_ptr<std::string> request) {
  authorizeConnection();
  folly::Promise<std::unique_ptr<std::string>> p;

  auto reply = requestReplyMessage(
      module, fbzmq::Message::from(std::move(*request)).value(), false);
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<std::string>(reply->read<std::string>().value()));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<bool>
OpenrCtrlHandler::semifuture_hasModule(thrift::OpenrModuleType type) {
  authorizeConnection();
  folly::Promise<bool> p;
  p.setValue(0 != moduleSockets_.count(type));
  return p.getSemiFuture();
}

facebook::fb303::cpp2::fb_status
OpenrCtrlHandler::getStatus() {
  return facebook::fb303::cpp2::fb_status::ALIVE;
}

folly::SemiFuture<std::unique_ptr<std::vector<fbzmq::thrift::EventLog>>>
OpenrCtrlHandler::semifuture_getEventLogs() {
  folly::Promise<std::unique_ptr<std::vector<fbzmq::thrift::EventLog>>> p;

  auto eventLogs = zmqMonitorClient_->getLastEventLogs();
  if (eventLogs.hasValue()) {
    p.setValue(std::make_unique<std::vector<fbzmq::thrift::EventLog>>(
        eventLogs.value()));
  } else {
    p.setException(
        thrift::OpenrError(std::string("Fail to retrieve eventlogs")));
  }

  return p.getSemiFuture();
}

void
OpenrCtrlHandler::getCounters(std::map<std::string, int64_t>& _return) {
  FacebookBase2::getCounters(_return);
  for (auto const& kv : zmqMonitorClient_->dumpCounters()) {
    _return.emplace(kv.first, static_cast<int64_t>(kv.second.value));
  }
}

void
OpenrCtrlHandler::getRegexCounters(
    std::map<std::string, int64_t>& _return,
    std::unique_ptr<std::string> regex) {
  // Compile regex
  re2::RE2 compiledRegex(*regex);
  if (not compiledRegex.ok()) {
    return;
  }

  // Get all counters
  std::map<std::string, int64_t> counters;
  getCounters(counters);

  // Filter counters
  for (auto const& kv : counters) {
    if (RE2::PartialMatch(kv.first, compiledRegex)) {
      _return.emplace(kv);
    }
  }
}

void
OpenrCtrlHandler::getSelectedCounters(
    std::map<std::string, int64_t>& _return,
    std::unique_ptr<std::vector<std::string>> keys) {
  // Get all counters
  std::map<std::string, int64_t> counters;
  getCounters(counters);

  // Filter counters
  for (auto const& key : *keys) {
    auto it = counters.find(key);
    if (it != counters.end()) {
      _return.emplace(*it);
    }
  }
}

int64_t
OpenrCtrlHandler::getCounter(std::unique_ptr<std::string> key) {
  auto counter = zmqMonitorClient_->getCounter(*key);
  if (counter.hasValue()) {
    return static_cast<int64_t>(counter->value);
  }
  return 0;
}

void
OpenrCtrlHandler::getMyNodeName(std::string& _return) {
  _return = std::string(nodeName_);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_advertisePrefixes(
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  thrift::PrefixManagerRequest request;
  request.cmd = thrift::PrefixManagerCommand::ADD_PREFIXES;
  request.prefixes = std::move(*prefixes);

  return processThriftRequest(
      thrift::OpenrModuleType::PREFIX_MANAGER,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_withdrawPrefixes(
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  thrift::PrefixManagerRequest request;
  request.cmd = thrift::PrefixManagerCommand::WITHDRAW_PREFIXES;
  request.prefixes = std::move(*prefixes);

  return processThriftRequest(
      thrift::OpenrModuleType::PREFIX_MANAGER,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_withdrawPrefixesByType(
    thrift::PrefixType prefixType) {
  thrift::PrefixManagerRequest request;
  request.cmd = thrift::PrefixManagerCommand::WITHDRAW_PREFIXES_BY_TYPE;
  request.type = prefixType;

  return processThriftRequest(
      thrift::OpenrModuleType::PREFIX_MANAGER,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_syncPrefixesByType(
    thrift::PrefixType prefixType,
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  thrift::PrefixManagerRequest request;
  request.cmd = thrift::PrefixManagerCommand::SYNC_PREFIXES_BY_TYPE;
  request.prefixes = std::move(*prefixes);
  request.type = prefixType;

  return processThriftRequest(
      thrift::OpenrModuleType::PREFIX_MANAGER,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
OpenrCtrlHandler::semifuture_getPrefixes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;

  thrift::PrefixManagerRequest request;
  request.cmd = thrift::PrefixManagerCommand::GET_ALL_PREFIXES;

  auto reply = requestReplyThrift<thrift::PrefixManagerResponse>(
      thrift::OpenrModuleType::PREFIX_MANAGER, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else if (not reply->success) {
    p.setException(thrift::OpenrError(reply->message));
  } else {
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
        std::move(reply->prefixes)));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
OpenrCtrlHandler::semifuture_getPrefixesByType(thrift::PrefixType prefixType) {
  folly::Promise<std::unique_ptr<std::vector<thrift::PrefixEntry>>> p;

  thrift::PrefixManagerRequest request;
  request.cmd = thrift::PrefixManagerCommand::GET_PREFIXES_BY_TYPE;
  request.type = prefixType;

  auto reply = requestReplyThrift<thrift::PrefixManagerResponse>(
      thrift::OpenrModuleType::PREFIX_MANAGER, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else if (not reply->success) {
    p.setException(thrift::OpenrError(reply->message));
  } else {
    p.setValue(std::make_unique<std::vector<thrift::PrefixEntry>>(
        std::move(reply->prefixes)));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
OpenrCtrlHandler::semifuture_getRouteDb() {
  folly::Promise<std::unique_ptr<thrift::RouteDatabase>> p;

  thrift::FibRequest request;
  request.cmd = thrift::FibCommand::ROUTE_DB_GET;

  auto reply = requestReplyThrift<thrift::RouteDatabase>(
      thrift::OpenrModuleType::FIB, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::RouteDatabase>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
OpenrCtrlHandler::semifuture_getRouteDbComputed(
    std::unique_ptr<std::string> nodeName) {
  folly::Promise<std::unique_ptr<thrift::RouteDatabase>> p;

  thrift::DecisionRequest request;
  request.cmd = thrift::DecisionCommand::ROUTE_DB_GET;
  request.nodeName = std::move(*nodeName);

  auto reply = requestReplyThrift<thrift::DecisionReply>(
      thrift::OpenrModuleType::DECISION, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::RouteDatabase>(std::move(reply->routeDb)));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>>
OpenrCtrlHandler::semifuture_getPerfDb() {
  folly::Promise<std::unique_ptr<thrift::PerfDatabase>> p;

  thrift::FibRequest request;
  request.cmd = thrift::FibCommand::PERF_DB_GET;

  auto reply = requestReplyThrift<thrift::PerfDatabase>(
      thrift::OpenrModuleType::FIB, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::PerfDatabase>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::AdjDbs>>
OpenrCtrlHandler::semifuture_getDecisionAdjacencyDbs() {
  folly::Promise<std::unique_ptr<thrift::AdjDbs>> p;

  thrift::DecisionRequest request;
  request.cmd = thrift::DecisionCommand::ADJ_DB_GET;

  auto reply = requestReplyThrift<thrift::DecisionReply>(
      thrift::OpenrModuleType::DECISION, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::AdjDbs>(std::move(reply->adjDbs)));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::PrefixDbs>>
OpenrCtrlHandler::semifuture_getDecisionPrefixDbs() {
  folly::Promise<std::unique_ptr<thrift::PrefixDbs>> p;

  thrift::DecisionRequest request;
  request.cmd = thrift::DecisionCommand::PREFIX_DB_GET;

  auto reply = requestReplyThrift<thrift::DecisionReply>(
      thrift::OpenrModuleType::DECISION, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::PrefixDbs>(std::move(reply->prefixDbs)));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::HealthCheckerInfo>>
OpenrCtrlHandler::semifuture_getHealthCheckerInfo() {
  folly::Promise<std::unique_ptr<thrift::HealthCheckerInfo>> p;

  thrift::HealthCheckerRequest request;
  request.cmd = thrift::HealthCheckerCmd::PEEK;

  auto reply = requestReplyThrift<thrift::HealthCheckerInfo>(
      thrift::OpenrModuleType::HEALTH_CHECKER, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::HealthCheckerInfo>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyVals(
    std::unique_ptr<std::vector<std::string>> filterKeys) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;

  thrift::KvStoreRequest request;
  thrift::KeyGetParams params;

  params.keys = std::move(*filterKeys);
  request.cmd = thrift::Command::KEY_GET;
  request.keyGetParams = params;

  auto reply = requestReplyThrift<thrift::Publication>(
      thrift::OpenrModuleType::KVSTORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::Publication>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyValsFiltered(
    std::unique_ptr<thrift::KeyDumpParams> filter) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;

  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams = std::move(*filter);

  auto reply = requestReplyThrift<thrift::Publication>(
      thrift::OpenrModuleType::KVSTORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::Publication>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreHashFiltered(
    std::unique_ptr<thrift::KeyDumpParams> filter) {
  folly::Promise<std::unique_ptr<thrift::Publication>> p;

  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::HASH_DUMP;
  request.keyDumpParams = std::move(*filter);

  auto reply = requestReplyThrift<thrift::Publication>(
      thrift::OpenrModuleType::KVSTORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::Publication>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<bool>
OpenrCtrlHandler::semifuture_longPollKvStoreAdj(
    std::unique_ptr<thrift::KeyVals> snapshot) {
  folly::Promise<bool> p;
  folly::SemiFuture<bool> sf = p.getSemiFuture();

  auto timeStamp = getUnixTimeStampMs();
  auto requestId = pendingRequestId_++;

  thrift::KvStoreRequest request;
  thrift::KeyDumpParams params;

  // Only care about "adj:" key
  params.prefix = Constants::kAdjDbMarker;
  // Only dump difference between KvStore and client snapshot
  params.keyValHashes = std::move(*snapshot);
  request.cmd = thrift::Command::KEY_DUMP;
  request.keyDumpParams = std::move(params);

  auto reply = requestReplyThrift<thrift::Publication>(
      thrift::OpenrModuleType::KVSTORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else if (
      reply->keyVals.size() or
      (reply->tobeUpdatedKeys.hasValue() and
       reply->tobeUpdatedKeys.value().size())) {
    VLOG(3) << "AdjKey hash change. Notify immediately";
    p.setValue(true);
  } else {
    // Client provided data is consistent with KvStore.
    // Store req for future processing when there is publication
    // from KvStore.
    longPollReqs_.withWLock([&](auto& longPollReq) {
      longPollReq.emplace(requestId, std::make_pair(std::move(p), timeStamp));
    });
  }
  return sf;
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  const bool solicitResponse = setParams->solicitResponse;
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::KEY_SET;
  request.keySetParams = std::move(*setParams);
  request.area = std::move(*area);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE,
      std::move(request),
      not solicitResponse);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setKvStoreKeyValsOneWay(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  setParams->solicitResponse = false; // Disable solicit response
  return semifuture_setKvStoreKeyVals(std::move(setParams), std::move(area));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_processKvStoreDualMessage(
    std::unique_ptr<thrift::DualMessages> messages) {
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::DUAL;
  request.dualMessages = std::move(*messages);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), true /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_updateFloodTopologyChild(
    std::unique_ptr<thrift::FloodTopoSetParams> params) {
  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::FLOOD_TOPO_SET;
  request.floodTopoSetParams = std::move(*params);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), true /* oneway */);
}

folly::SemiFuture<std::unique_ptr<thrift::SptInfos>>
OpenrCtrlHandler::semifuture_getSpanningTreeInfos() {
  folly::Promise<std::unique_ptr<thrift::SptInfos>> p;

  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::FLOOD_TOPO_GET;

  auto reply = requestReplyThrift<thrift::SptInfos>(
      thrift::OpenrModuleType::KVSTORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::SptInfos>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_addUpdateKvStorePeers(
    std::unique_ptr<thrift::PeersMap> peers) {
  thrift::KvStoreRequest request;
  thrift::PeerAddParams params;

  params.peers = std::move(*peers);
  request.cmd = thrift::Command::PEER_ADD;
  request.peerAddParams = params;

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_deleteKvStorePeers(
    std::unique_ptr<std::vector<std::string>> peerNames) {
  thrift::KvStoreRequest request;
  thrift::PeerDelParams params;

  params.peerNames = std::move(*peerNames);
  request.cmd = thrift::Command::PEER_DEL;
  request.peerDelParams = params;

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), false /* oneway */);
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
OpenrCtrlHandler::semifuture_getKvStorePeers() {
  folly::Promise<std::unique_ptr<thrift::PeersMap>> p;

  thrift::KvStoreRequest request;
  request.cmd = thrift::Command::PEER_DUMP;

  auto reply = requestReplyThrift<thrift::PeerCmdReply>(
      thrift::OpenrModuleType::KVSTORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::PeersMap>(std::move(reply->peers)));
  }

  return p.getSemiFuture();
}

apache::thrift::Stream<thrift::Publication>
OpenrCtrlHandler::subscribeKvStore() {
  // Get new client-ID (monotonically increasing)
  auto clientToken = publisherToken_++;

  auto streamAndPublisher =
      createStreamPublisher<thrift::Publication>([this, clientToken]() {
        SYNCHRONIZED(kvStorePublishers_) {
          if (kvStorePublishers_.erase(clientToken)) {
            LOG(INFO) << "KvStore snoop stream-" << clientToken << " ended.";
          } else {
            LOG(ERROR) << "Can't remove unknown KvStore snoop stream-"
                       << clientToken;
          }
        }
      });

  SYNCHRONIZED(kvStorePublishers_) {
    assert(kvStorePublishers_.count(clientToken) == 0);
    LOG(INFO) << "KvStore snoop stream-" << clientToken << " started.";
    kvStorePublishers_.emplace(
        clientToken, std::move(streamAndPublisher.second));
  }
  return std::move(streamAndPublisher.first);
}

folly::SemiFuture<
    apache::thrift::ResponseAndStream<thrift::Publication, thrift::Publication>>
OpenrCtrlHandler::semifuture_subscribeAndGetKvStore() {
  return semifuture_getKvStoreKeyValsFiltered(
             std::make_unique<thrift::KeyDumpParams>())
      .defer(
          [stream = subscribeKvStore()](
              folly::Try<std::unique_ptr<thrift::Publication>>&& pub) mutable {
            pub.throwIfFailed();
            return apache::thrift::
                ResponseAndStream<thrift::Publication, thrift::Publication>{
                    std::move(*pub.value()), std::move(stream)};
          });
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setNodeOverload() {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::SET_OVERLOAD;

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetNodeOverload() {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::UNSET_OVERLOAD;

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setInterfaceOverload(
    std::unique_ptr<std::string> interfaceName) {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::SET_LINK_OVERLOAD;
  request.interfaceName = std::move(*interfaceName);

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetInterfaceOverload(
    std::unique_ptr<std::string> interfaceName) {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::UNSET_LINK_OVERLOAD;
  request.interfaceName = std::move(*interfaceName);

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setInterfaceMetric(
    std::unique_ptr<std::string> interfaceName, int32_t overrideMetric) {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::SET_LINK_METRIC;
  request.interfaceName = std::move(*interfaceName);
  request.overrideMetric = overrideMetric;

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetInterfaceMetric(
    std::unique_ptr<std::string> interfaceName) {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::UNSET_LINK_METRIC;
  request.interfaceName = std::move(*interfaceName);

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setAdjacencyMetric(
    std::unique_ptr<std::string> interfaceName,
    std::unique_ptr<std::string> adjNodeName,
    int32_t overrideMetric) {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::SET_ADJ_METRIC;
  request.interfaceName = std::move(*interfaceName);
  request.adjNodeName = std::move(*adjNodeName);
  request.overrideMetric = overrideMetric;

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetAdjacencyMetric(
    std::unique_ptr<std::string> interfaceName,
    std::unique_ptr<std::string> adjNodeName) {
  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::UNSET_ADJ_METRIC;
  request.interfaceName = std::move(*interfaceName);
  request.adjNodeName = std::move(*adjNodeName);

  return processThriftRequest(
      thrift::OpenrModuleType::LINK_MONITOR,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
OpenrCtrlHandler::semifuture_getInterfaces() {
  folly::Promise<std::unique_ptr<thrift::DumpLinksReply>> p;

  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::DUMP_LINKS;

  auto reply = requestReplyThrift<thrift::DumpLinksReply>(
      thrift::OpenrModuleType::LINK_MONITOR, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::DumpLinksReply>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::OpenrVersions>>
OpenrCtrlHandler::semifuture_getOpenrVersion() {
  folly::Promise<std::unique_ptr<thrift::OpenrVersions>> p;

  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::GET_VERSION;

  auto reply = requestReplyThrift<thrift::OpenrVersions>(
      thrift::OpenrModuleType::LINK_MONITOR, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::OpenrVersions>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<std::unique_ptr<thrift::BuildInfo>>
OpenrCtrlHandler::semifuture_getBuildInfo() {
  folly::Promise<std::unique_ptr<thrift::BuildInfo>> p;

  thrift::LinkMonitorRequest request;
  request.cmd = thrift::LinkMonitorCommand::GET_BUILD_INFO;

  auto reply = requestReplyThrift<thrift::BuildInfo>(
      thrift::OpenrModuleType::LINK_MONITOR, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::BuildInfo>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setConfigKey(
    std::unique_ptr<std::string> key, std::unique_ptr<std::string> value) {
  thrift::StoreRequest request;
  request.requestType = thrift::StoreRequestType::STORE;
  request.key = std::move(*key);
  request.data = std::move(*value);

  return processThriftRequest(
      thrift::OpenrModuleType::PERSISTENT_STORE,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_eraseConfigKey(std::unique_ptr<std::string> key) {
  thrift::StoreRequest request;
  request.requestType = thrift::StoreRequestType::ERASE;
  request.key = std::move(*key);

  return processThriftRequest(
      thrift::OpenrModuleType::PERSISTENT_STORE,
      std::move(request),
      false /* oneway */);
}

folly::SemiFuture<std::unique_ptr<std::string>>
OpenrCtrlHandler::semifuture_getConfigKey(std::unique_ptr<std::string> key) {
  folly::Promise<std::unique_ptr<std::string>> p;

  thrift::StoreRequest request;
  request.requestType = thrift::StoreRequestType::LOAD;
  request.key = std::move(*key);

  auto reply = requestReplyThrift<thrift::StoreResponse>(
      thrift::OpenrModuleType::PERSISTENT_STORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else if (not reply->success) {
    p.setException(thrift::OpenrError("key not found"));
  } else {
    p.setValue(std::make_unique<std::string>(std::move(reply->data)));
  }

  return p.getSemiFuture();
}

} // namespace openr
