/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <wangle/ssl/SSLContextConfig.h>

#include <openr/common/Types.h>
#include <openr/config/Config.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/messaging/Queue.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {
struct PluginArgs {
  messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate>& staticRouteUpdatesQueue;
  messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueue;
  std::shared_ptr<const Config> config;
  std::shared_ptr<wangle::SSLContextConfig> sslContext;
};

void pluginStart(const PluginArgs& /* pluginArgs */);
void pluginStop();
void vipPluginStart(const PluginArgs& /* PluginArgs */);
void vipPluginStop();
} // namespace openr
