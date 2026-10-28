/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>

#include <openr/common/LsdbTypes.h>
#include <openr/common/Types.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/dispatcher/DispatcherQueue.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>
#include <openr/nl/NetlinkProtocolSocket.h>

namespace openr::e2e {

struct OpenrModuleReaders {
  messaging::RQueue<DecisionRouteUpdate> fibRoutes;
  messaging::RQueue<DecisionRouteUpdate> decisionStaticRoutes;
  messaging::RQueue<DecisionRouteUpdate> prefixManagerFibRoutes;
  messaging::RQueue<thrift::InitializationEvent> sparkInitialization;
  messaging::RQueue<InterfaceDatabase> sparkInterfaces;
  messaging::RQueue<NeighborInitEvent> linkMonitorNeighbors;
  messaging::RQueue<PrefixEvent> prefixManagerPrefixes;
  messaging::RQueue<PeerEvent> kvStorePeers;
  messaging::RQueue<PeerEvent> decisionPeers;
  messaging::RQueue<KeyValueRequest> kvStoreRequests;
  messaging::RQueue<fbnl::NetlinkEvent> linkMonitorNetlink;
  messaging::RQueue<AddressEvent> sparkAddresses;
};

struct OpenrDispatcherReaders {
  messaging::RQueue<KvStorePublication> decision;
  messaging::RQueue<KvStorePublication> prefixManager;
  messaging::RQueue<KvStorePublication> linkMonitor;
};

class OpenrQueueBundle final {
 public:
  OpenrQueueBundle() = default;
  OpenrQueueBundle(const OpenrQueueBundle&) = delete;
  OpenrQueueBundle& operator=(const OpenrQueueBundle&) = delete;

  OpenrModuleReaders makeModuleReaders(bool enableQueueCoalescing);
  OpenrDispatcherReaders makeDispatcherReaders(bool enableQueueCoalescing);
  void close();

  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdates;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdates;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdates;
  messaging::ReplicateQueue<thrift::InitializationEvent>
      prefixManagerInitializationEvents;
  messaging::ReplicateQueue<InterfaceDatabase> interfaceUpdates;
  messaging::ReplicateQueue<NeighborInitEvent> neighborUpdates;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdates;
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdates;
  messaging::ReplicateQueue<PeerEvent> peerUpdates;
  messaging::ReplicateQueue<KeyValueRequest> kvRequests;
  messaging::ReplicateQueue<fbnl::NetlinkEvent> netlinkEvents;
  messaging::ReplicateQueue<AddressEvent> addressEvents;
  messaging::ReplicateQueue<LogSample> logSamples;
  DispatcherQueue dispatchedPublications;

 private:
  std::atomic<bool> closed_{false};
};

} // namespace openr::e2e
