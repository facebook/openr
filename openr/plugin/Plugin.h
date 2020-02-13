/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/zmq/Zmq.h>
#include <wangle/ssl/SSLContextConfig.h>

#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/messaging/Queue.h>

namespace openr {
struct PluginArgs {
  std::string myNodeName;
  fbzmq::Context& zmqContext;
  PrefixManagerLocalCmdUrl prefixManagerUrl;
  messaging::RQueue<thrift::RouteDatabaseDelta> routeUpdatesQueue;
  bool enableSegmentRouting{false};
  std::shared_ptr<wangle::SSLContextConfig> sslContext;
};

void pluginStart(const PluginArgs& /* pluginArgs */);
void pluginStop();
} // namespace openr
