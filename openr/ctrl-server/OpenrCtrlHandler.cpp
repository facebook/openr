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

template <typename ReturnType, typename InputType>
folly::Expected<ReturnType, fbzmq::Error>
OpenrCtrlHandler::requestReply(
    thrift::OpenrModuleType module, InputType&& input) {
  // Check module
  auto moduleIt = moduleSockets_.find(module);
  if (moduleIt == moduleSockets_.end()) {
    return folly::makeUnexpected(fbzmq::Error(
        0, folly::sformat("Unknown module {}", static_cast<int>(module))));
  }

  // Send request
  auto& sock = moduleIt->second;
  apache::thrift::CompactSerializer serializer;
  auto sendRet = sock.sendThriftObj(input, serializer);
  if (sendRet.hasError()) {
    return folly::makeUnexpected(sendRet.error());
  }

  // Recv response
  return sock.recvThriftObj<ReturnType>(serializer, Constants::kReadTimeout);
}

folly::SemiFuture<std::unique_ptr<std::string>>
OpenrCtrlHandler::semifuture_command(
    thrift::OpenrModuleType type, std::unique_ptr<std::string> request) {
  authorizeConnection();
  folly::Promise<std::unique_ptr<std::string>> p;
  try {
    auto& sock = moduleSockets_.at(type);
    sock.sendOne(fbzmq::Message::from(*request).value()).value();
    std::string response = sock.recvOne(Constants::kReadTimeout)
                               .value()
                               .read<std::string>()
                               .value();
    p.setValue(std::make_unique<std::string>(std::move(response)));
  } catch (const std::out_of_range& e) {
    auto error = folly::sformat("Unknown module: {}", static_cast<int>(type));
    LOG(ERROR) << error;
    p.setException(thrift::OpenrError(error));
  } catch (const folly::Unexpected<fbzmq::Error>::BadExpectedAccess& e) {
    auto error = "Error processing request: " + e.error().errString;
    LOG(ERROR) << error;
    p.setException(thrift::OpenrError(error));
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

  auto reply = requestReply<thrift::RouteDatabase>(
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

  auto reply = requestReply<thrift::DecisionReply>(
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

  auto reply = requestReply<thrift::PerfDatabase>(
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

  auto reply = requestReply<thrift::DecisionReply>(
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

  auto reply = requestReply<thrift::DecisionReply>(
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

  auto reply = requestReply<thrift::HealthCheckerInfo>(
      thrift::OpenrModuleType::HEALTH_CHECKER, std::move(request));
  if (reply.hasError()) {
    p.setException(thrift::OpenrError(reply.error().errString));
  } else {
    p.setValue(
        std::make_unique<thrift::HealthCheckerInfo>(std::move(reply.value())));
  }

  return p.getSemiFuture();
}

} // namespace openr
