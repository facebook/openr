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
  void start(
      std::shared_ptr<const Config> config,
      std::shared_ptr<openr::OpenrCtrlHandler>& handler,
      std::shared_ptr<wangle::SSLContextConfig> sslContext);
  void stop();

 private:
  std::unique_ptr<apache::thrift::ThriftServer> thriftCtrlServer_;
  std::thread thriftCtrlServerThread_;
};

} // namespace openr
