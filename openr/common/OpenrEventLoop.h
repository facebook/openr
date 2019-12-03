/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/OpenrModule.h>

namespace openr {

//
// OpenrEventLoop is an OpenrModule supported by ZmqEventLoop
//

class OpenrEventLoop : public OpenrModule, public fbzmq::ZmqEventLoop {
 public:
  std::chrono::seconds
  getTimestamp() const override {
    return fbzmq::ZmqEventLoop::getTimestamp();
  }

 protected:
  OpenrEventLoop(
      const std::string& nodeName,
      const thrift::OpenrModuleType type,
      fbzmq::Context& zmqContext)
      : OpenrModule(nodeName, type, zmqContext) {
    runInEventLoop([this]() {
      prepareSocket(inprocCmdSock_, inprocCmdUrl);
      addSocket(
          fbzmq::RawZmqSocketPtr{*inprocCmdSock_},
          ZMQ_POLLIN,
          [this](int) noexcept { processCmdSocketRequest(inprocCmdSock_); });
    });
  }
}; // class OpenrEventLoop

} // namespace openr
