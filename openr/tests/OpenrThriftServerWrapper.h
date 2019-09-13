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
#include <thrift/lib/cpp2/util/ScopedServerThread.h>
#include <thread>

namespace openr {

class OpenrThriftServerWrapper {
 private:
  fbzmq::ZmqEventLoop mainEvl_;
  std::thread mainEvlThread_;
  std::shared_ptr<apache::thrift::concurrency::ThreadManager> tm_{nullptr};
  apache::thrift::util::ScopedServerThread openrCtrlThriftServerThread_;
  std::string const nodeName_;
  MonitorSubmitUrl const monitorSubmitUrl_;
  KvStoreLocalPubUrl const kvStoreLocalPubUrl_;
  fbzmq::Context& context_;

  std::unordered_map<thrift::OpenrModuleType, std::shared_ptr<OpenrEventLoop>>
      moduleTypeToEvl_;
  std::shared_ptr<OpenrCtrlHandler> openrCtrlHandler_{nullptr};

 public:
  OpenrThriftServerWrapper(
      std::string const& nodeName,
      MonitorSubmitUrl const& monitorSubmitUrl,
      KvStoreLocalPubUrl const& kvstoreLocalPubUrl,
      fbzmq::Context& context);

  // start Open/R thrift server
  void run();

  // stop Open/R thrift server
  void stop();

  inline void
  addModuleType(
      thrift::OpenrModuleType module, std::shared_ptr<OpenrEventLoop> evl) {
    moduleTypeToEvl_[module] = evl;
  }

  inline uint16_t
  getOpenrCtrlThriftPort() {
    return openrCtrlThriftServerThread_.getAddress()->getPort();
  }

  inline std::shared_ptr<OpenrCtrlHandler>&
  getOpenrCtrlHandler() {
    return openrCtrlHandler_;
  }
};

} // namespace openr
