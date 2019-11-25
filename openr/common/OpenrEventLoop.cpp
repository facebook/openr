/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/OpenrEventLoop.h"

#include <folly/Format.h>

namespace openr {

OpenrEventLoop::OpenrEventLoop(
    const std::string& nodeName,
    const thrift::OpenrModuleType type,
    fbzmq::Context& zmqContext)
    : moduleType(type),
      moduleName(apache::thrift::TEnumTraits<thrift::OpenrModuleType>::findName(
          moduleType)),
      inprocCmdUrl(
          folly::sformat("inproc://{}_{}_local_cmd", nodeName, moduleName)),
      inprocCmdSock_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}) {
  runInEventLoop([this]() { prepareSocket(inprocCmdSock_, inprocCmdUrl); });
}

void
OpenrEventLoop::prepareSocket(
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& socket,
    std::string const& url,
    folly::Optional<int> maybeIpTos) {
  std::vector<std::pair<int, int>> socketOptions{
      {ZMQ_SNDHWM, Constants::kHighWaterMark},
      {ZMQ_RCVHWM, Constants::kHighWaterMark},
      {ZMQ_SNDTIMEO, Constants::kReadTimeout.count()},
      {ZMQ_ROUTER_HANDOVER, 1},
      {ZMQ_TCP_KEEPALIVE, Constants::kKeepAliveEnable},
      {ZMQ_TCP_KEEPALIVE_IDLE, Constants::kKeepAliveTime.count()},
      {ZMQ_TCP_KEEPALIVE_CNT, Constants::kKeepAliveCnt},
      {ZMQ_TCP_KEEPALIVE_INTVL, Constants::kKeepAliveIntvl.count()}};

  if (maybeIpTos.hasValue()) {
    socketOptions.emplace_back(ZMQ_TOS, maybeIpTos.value());
  }

  for (const auto& pair : socketOptions) {
    const auto opt = pair.first;
    const auto val = pair.second;
    auto rc = socket.setSockOpt(opt, &val, sizeof(val));
    if (rc.hasError()) {
      LOG(FATAL) << "Error setting zmq opt: " << opt << "to " << val
                 << ". Error: " << rc.error();
    }
  }

  auto rc = socket.bind(fbzmq::SocketUrl{url});
  if (rc.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << url << "'. Error: " << rc.error();
  }

  addSocket(
      fbzmq::RawZmqSocketPtr{*socket},
      ZMQ_POLLIN,
      [this, &socket](int) noexcept { processCmdSocketRequest(socket); });
}

void
OpenrEventLoop::processCmdSocketRequest(
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock) noexcept {
  auto maybeReq = cmdSock.recvMultiple();

  if (maybeReq.hasError()) {
    LOG(ERROR) << "OpenrEventLoop::processCmdSocketRequest: Error receiving "
               << "command: " << maybeReq.error();
    return;
  }

  auto req = maybeReq.value();
  if (req.empty()) {
    LOG(ERROR) << "Empty request received";
    return;
  }
  auto maybeReply = processRequestMsg(std::move(req.back()));
  req.pop_back();

  // All messages of the multipart request except the last are sent back as they
  // are ids or empty delims. Add the response at the end of that list.
  if (maybeReply.hasValue()) {
    req.emplace_back(std::move(maybeReply.value()));
  } else {
    req.emplace_back(
        fbzmq::Message::from(Constants::kErrorResponse.toString()).value());
  }

  if (!(thrift::OpenrModuleType::KVSTORE == moduleType && req.back().empty())) {
    auto sndRet = cmdSock.sendMultiple(req);
    if (sndRet.hasError()) {
      LOG(ERROR) << "Error sending response. " << sndRet.error();
    }
  }
  return;
}

} // namespace openr
