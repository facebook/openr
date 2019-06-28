/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/zmq/Zmq.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thread>

namespace openr {

class OpenrModuleTestBase {
 private:
  fbzmq::ZmqEventLoop mainEvl_;
  std::thread mainEvlThread_;
  std::shared_ptr<apache::thrift::concurrency::ThreadManager> tm_;

 protected:
  /*
   * Variable can be manipulated by inherited class
   */
  std::shared_ptr<OpenrCtrlHandler> openrCtrlHandler_;
  std::unordered_map<thrift::OpenrModuleType, std::shared_ptr<OpenrEventLoop>>
      moduleTypeToEvl_;

 public:
  /*
   * Method to start/stop OpenrCtrlHandler
   */
  void startOpenrCtrlHandler(
      const std::string& nodeName,
      const std::unordered_set<std::string>& acceptablePeerNames,
      MonitorSubmitUrl const& monitorSubmitUrl,
      KvStoreLocalPubUrl const& kvStoreLocalPubUrl,
      fbzmq::Context& context);

  void stopOpenrCtrlHandler();
};

} // namespace openr
