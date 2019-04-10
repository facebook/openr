/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/ctrl-server/OpenrCtrlHandler.h>
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
    : nodeName_(nodeName),
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

void
OpenrCtrlHandler::command(
    std::string& response,
    thrift::OpenrModuleType type,
    std::unique_ptr<std::string> request) {
  authorizeConnection();
  try {
    auto& sock = moduleSockets_.at(type);
    sock.sendOne(fbzmq::Message::from(*request).value()).value();
    response = sock.recvOne(Constants::kReadTimeout)
                   .value()
                   .read<std::string>()
                   .value();
  } catch (const std::out_of_range& e) {
    auto error = folly::sformat("Unknown module: {}", static_cast<int>(type));
    LOG(ERROR) << error;
    throw thrift::OpenrError(error);
  } catch (const folly::Unexpected<fbzmq::Error>::BadExpectedAccess& e) {
    auto error = "Error processing request: " + e.error().errString;
    LOG(ERROR) << error;
    throw thrift::OpenrError(error);
  }
}

bool
OpenrCtrlHandler::hasModule(thrift::OpenrModuleType type) {
  authorizeConnection();
  return 0 != moduleSockets_.count(type);
}
} // namespace openr
