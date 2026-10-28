/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrQueueBundle.h>

#include <optional>

#include <openr/common/Constants.h>
#include <openr/config/Config.h>
#include <openr/dispatcher/PublicationCoalescer.h>
#include <openr/kvstore/KvStoreRequestQueue.h>
#include <openr/prefix-manager/PrefixUpdatesQueue.h>

namespace openr::e2e {

OpenrModuleReaders
OpenrQueueBundle::makeModuleReaders(bool enableQueueCoalescing) {
  return {
      routeUpdates.getReader("fibDecision", coalesceDecisionRouteUpdates),
      staticRouteUpdates.getReader(
          "decision",
          enableQueueCoalescing ? coalesceDecisionRouteUpdates : nullptr),
      fibRouteUpdates.getReader(
          "routeUpdates",
          enableQueueCoalescing ? coalesceDecisionRouteUpdates : nullptr),
      prefixManagerInitializationEvents.getReader("spark"),
      interfaceUpdates.getReader("spark"),
      neighborUpdates.getReader("linkMonitor"),
      getPrefixUpdatesQueueReader(prefixUpdates, enableQueueCoalescing),
      peerUpdates.getReader("kvStore"),
      peerUpdates.getReader("decision"),
      getKvStoreRequestQueueReader(kvRequests, enableQueueCoalescing),
      netlinkEvents.getReader("linkMonitor"),
      addressEvents.getReader("spark"),
  };
}

OpenrDispatcherReaders
OpenrQueueBundle::makeDispatcherReaders(bool enableQueueCoalescing) {
  std::optional<messaging::StateSuppressionPolicy<KvStorePublication>> policy;
  if (enableQueueCoalescing) {
    policy = getKvStorePublicationSuppressionPolicy();
  }
  return {
      dispatchedPublications.getReader(
          {Constants::kAdjDbMarker.toString(),
           Constants::kPrefixDbMarker.toString(),
           FabricConfig::kDrainStatusMarker.toString()},
          "decision",
          policy),
      dispatchedPublications.getReader(
          {Constants::kPrefixDbMarker.toString()}, "prefixManager", policy),
      dispatchedPublications.getReader({"link-monitor"}, "linkMonitor", policy),
  };
}

void
OpenrQueueBundle::close() {
  if (closed_.exchange(true)) {
    return;
  }
  routeUpdates.close();
  staticRouteUpdates.close();
  fibRouteUpdates.close();
  prefixManagerInitializationEvents.close();
  interfaceUpdates.close();
  neighborUpdates.close();
  prefixUpdates.close();
  kvStoreUpdates.close();
  peerUpdates.close();
  kvRequests.close();
  netlinkEvents.close();
  addressEvents.close();
  logSamples.close();
  dispatchedPublications.close();
}

} // namespace openr::e2e
