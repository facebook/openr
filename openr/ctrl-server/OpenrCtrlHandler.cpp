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
#include <openr/common/Constants.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

namespace openr {

OpenrCtrlHandler::OpenrCtrlHandler(
    const std::string& nodeName,
    const std::unordered_set<std::string>& acceptablePeerCommonNames,
    std::unordered_map<
        thrift::OpenrModuleType,
        std::shared_ptr<OpenrEventLoop>>& moduleTypeToEvl,
    MonitorSubmitUrl const& monitorSubmitUrl,
    fbzmq::Context& context)
    : facebook::fb303::FacebookBase2("openr"),
      nodeName_(nodeName),
      acceptablePeerCommonNames_(acceptablePeerCommonNames),
      moduleTypeToEvl_(moduleTypeToEvl) {
  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(context, monitorSubmitUrl);

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

OpenrCtrlHandler::~OpenrCtrlHandler() {}

void
OpenrCtrlHandler::authorizeConnection() {
  auto connContext = getConnectionContext()->getConnectionContext();
  auto peerCommonName = connContext->getPeerCommonName();

  if (peerCommonName.empty() || acceptablePeerCommonNames_.empty()) {
    // for now, we will allow non-secure connections, but lets log the event so
    // we know how often this is happening.
    fbzmq::LogSample sample{};

    sample.addString(
        "event",
        peerCommonName.empty() ? "UNENCRYPTED_CTRL_CONNECTION"
                               : "UNRESTRICTED_AUTHORIZATION");
    sample.addString("entity", "OPENR_CTRL_HANDLER");
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
  request.cmd = thrift::DecisionCommand::ROUTE_DB_GET;

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

  thrift::Request request;
  request.cmd = thrift::Command::KEY_GET;
  request.keyGetParams.keys = std::move(*filterKeys);

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

  thrift::Request request;
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

  thrift::Request request;
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

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams) {
  const bool solicitResponse = setParams->solicitResponse;
  thrift::Request request;
  request.cmd = thrift::Command::KEY_SET;
  request.keySetParams = std::move(*setParams);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE,
      std::move(request),
      not solicitResponse);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setKvStoreKeyValsOneWay(
    std::unique_ptr<thrift::KeySetParams> setParams) {
  setParams->solicitResponse = false; // Disable solicit response
  return semifuture_setKvStoreKeyVals(std::move(setParams));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_processKvStoreDualMessage(
    std::unique_ptr<thrift::DualMessages> messages) {
  thrift::Request request;
  request.cmd = thrift::Command::DUAL;
  request.dualMessages = std::move(*messages);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), true /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_updateFloodTopologyChild(
    std::unique_ptr<thrift::FloodTopoSetParams> params) {
  thrift::Request request;
  request.cmd = thrift::Command::FLOOD_TOPO_SET;
  request.floodTopoSetParams = std::move(*params);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), true /* oneway */);
}

folly::SemiFuture<std::unique_ptr<thrift::SptInfo>>
OpenrCtrlHandler::semifuture_getSpanningTreeInfo() {
  folly::Promise<std::unique_ptr<thrift::SptInfo>> p;

  thrift::Request request;
  request.cmd = thrift::Command::FLOOD_TOPO_GET;

  auto reply = requestReplyThrift<thrift::SptInfo>(
      thrift::OpenrModuleType::KVSTORE, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(std::make_unique<thrift::SptInfo>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_addUpdateKvStorePeers(
    std::unique_ptr<thrift::PeersMap> peers) {
  thrift::Request request;
  request.cmd = thrift::Command::PEER_ADD;
  request.peerAddParams.peers = std::move(*peers);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), false /* oneway */);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_deleteKvStorePeers(
    std::unique_ptr<std::vector<std::string>> peerNames) {
  thrift::Request request;
  request.cmd = thrift::Command::PEER_DEL;
  request.peerDelParams.peerNames = std::move(*peerNames);

  return processThriftRequest(
      thrift::OpenrModuleType::KVSTORE, std::move(request), false /* oneway */);
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
OpenrCtrlHandler::semifuture_getKvStorePeers() {
  folly::Promise<std::unique_ptr<thrift::PeersMap>> p;

  thrift::Request request;
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

} // namespace openr
