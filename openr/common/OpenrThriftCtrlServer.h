/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

namespace openr {

/**
 * This class is for Open/R thrift server, including:
 *  1. Start one thrift server,
 *  2. Stop the thrift server.
 * You could design your own server implementation.
 */
class OpenrThriftCtrlServer {
 public:
  OpenrThriftCtrlServer(
      std::shared_ptr<const Config> config,
      std::shared_ptr<openr::OpenrCtrlHandler>& handler,
      std::shared_ptr<wangle::SSLContextConfig> sslContext);

  // This will start the default thrift server and thread.
  // You could design your own server implementation.
  void start();

  // Stop all servers and threads.
  void stop();

 private:
  // Helper function to start the server if you need to define different
  // behavior.
  void startDefaultThriftServer();
  void startNonDefaultThriftServer();
  void startVrfThread(
      bool isDefaultVrf, std::unique_ptr<apache::thrift::ThriftServer> server);
  std::unique_ptr<apache::thrift::ThriftServer> setUpThriftServer();

  // Vector for all thrift severs and their threads
  std::vector<std::unique_ptr<apache::thrift::ThriftServer>>
      thriftCtrlServerVec_;
  std::vector<std::thread> thriftCtrlServerThreadVec_;

  std::shared_ptr<const Config> config_;
  std::shared_ptr<openr::OpenrCtrlHandler> ctrlHandler_;
  std::shared_ptr<wangle::SSLContextConfig> sslContext_;
};

} // namespace openr
