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
#include <openr/common/OpenrModule.h>
#include <openr/if/gen-cpp2/OpenrCtrl_types.h>

namespace openr {

//
// OpenrModule is an abstract class that defines the essential api and
// members of all openr modules. Specifically, every openr module:
//
// 1. is a Runnable
// 2. has a name
// 3. should implement a request reply api on at least a inproc bound
//    ZMQ_ROUTER socket (and maybe other ZMQ sockets, like a globally
//    reachable one)
// 4. expects pollSocket API to be implemented
//

class OpenrModule : public virtual fbzmq::Runnable {
 public:
  const thrift::OpenrModuleType moduleType;
  const std::string moduleName;
  const std::string inprocCmdUrl;

  virtual std::chrono::seconds getTimestamp() const = 0;

 protected:
  OpenrModule(
      const std::string& nodeName,
      const thrift::OpenrModuleType type,
      fbzmq::Context& zmqContext);

  virtual folly::Expected<fbzmq::Message, fbzmq::Error> processRequestMsg(
      fbzmq::Message&& request) = 0;

  void prepareSocket(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& socket,
      std::string const& url,
      folly::Optional<int> maybeIpTos = folly::none);

  void processCmdSocketRequest(
      fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock) noexcept;

  // REQ/REP socket for communicating with the module
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> inprocCmdSock_;

 private:
  // disable copying
  OpenrModule(OpenrModule const&) = delete;
  OpenrModule& operator=(OpenrModule const&) = delete;
}; // class OpenrModule

} // namespace openr
