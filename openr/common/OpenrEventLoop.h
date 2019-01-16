/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Expected.h>
#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>

namespace openr {

//
// OpenrEventLoop is an abstract class that defines the essential api and
// members of all openr modules. Specifically, every openr module:
//
// 1. is a ZmqEventLoop
// 2. has a name
// 3. should implement a request reply api on at least a inproc bound
//    ZMQ_ROUTER socket (and maybe other ZMQ sockets, like a globally
//    reachable one)
//

class OpenrEventLoop : public fbzmq::ZmqEventLoop {
 public:
  const thrift::OpenrModuleType moduleType;
  const std::string moduleName;
  const std::string inprocCmdUrl;
  const folly::Optional<std::string> tcpCmdUrl;
  const folly::Optional<std::string> ipcCmdUrl;

 protected:
  OpenrEventLoop(
      const std::string& nodeName,
      const thrift::OpenrModuleType type,
      fbzmq::Context& zmqContext,
      folly::Optional<std::string> tcpUrl = folly::none,
      folly::Optional<std::string> ipcUrl = folly::none,
      folly::Optional<int> maybeIpTos = folly::none,
      int socketHwm = Constants::kHighWaterMark);

 private:
  // disable copying
  OpenrEventLoop(OpenrEventLoop const&) = delete;
  OpenrEventLoop& operator=(OpenrEventLoop const&) = delete;

  void prepareSocket(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& socket,
      std::string url,
      const std::vector<std::pair<int, int>>& socketOptions);

  void processCmdSocketRequest(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock) noexcept;

  virtual folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      fbzmq::Message&& request) = 0;

  // For backward compatibility, we are preserving the endpoints that the
  // modules previously had. All had inproc sockets while some also had tpc or
  // ipc sockets. going foraward, we will hopefully remove the tcp and ipc
  // sockets and shift any requests coming from outside the process to use
  // secure thrift
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> inprocCmdSock_;

  folly::Optional<fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>> tcpCmdSock_;

  folly::Optional<fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>> ipcCmdSock_;

}; // class OpenrEventLoop
} // namespace openr
