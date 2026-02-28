/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

/*
 * Represents a virtual interface on a simulated router.
 * Used to model the network topology for scale testing.
 */
struct VirtualInterface {
  std::string ifName;
  int ifIndex{0};
  folly::CIDRNetwork v6Addr;
  std::optional<folly::CIDRNetwork> v4Addr;
  bool isUp{true};

  /*
   * Connected peer information for this interface.
   * Maps to another router's interface.
   */
  std::string connectedRouterName;
  std::string connectedIfName;
  int32_t latencyMs{1};
  int32_t metric{1};
};

/*
 * Represents a link/adjacency between two virtual routers.
 * This is a directed edge in the topology graph.
 */
struct VirtualAdjacency {
  std::string localIfName;
  std::string remoteRouterName;
  std::string remoteIfName;
  int32_t metric{1};
  int32_t latencyMs{1};
  bool isUp{true};
};

/*
 * Lightweight representation of a simulated router for scale testing.
 *
 * This struct maintains the minimal state needed to simulate an OpenR peer.
 * Memory footprint is kept small (~1KB per router) to support 4000+ instances.
 *
 * The VirtualRouter can operate in two modes:
 * 1. KvStore injection mode: State is injected directly into KvStore
 * 2. Spark simulation mode: Full protocol simulation with hello/heartbeat
 */
struct VirtualRouter {
  /*
   * Unique node identifier - must match OpenR naming conventions.
   * Examples: "node-0", "1-2-3" (fabric topology), "42" (grid topology)
   */
  std::string nodeName;

  /*
   * Numeric ID for efficient lookups and label generation.
   */
  int nodeId{0};

  /*
   * OpenR area this router belongs to.
   * Defaults to the standard testing area.
   */
  std::string area{"area0"};

  /*
   * Interfaces on this router.
   * Each interface connects to one neighbor.
   */
  std::vector<VirtualInterface> interfaces;

  /*
   * Adjacencies formed with other routers.
   * This is derived from interfaces but stored separately for convenience.
   */
  std::vector<VirtualAdjacency> adjacencies;

  /*
   * Prefixes advertised by this router.
   */
  std::vector<thrift::PrefixEntry> advertisedPrefixes;

  /*
   * State tracking for Spark protocol simulation.
   * Only used when running in full protocol simulation mode.
   */
  uint64_t helloSeqNum{0};
  uint64_t heartbeatSeqNum{0};
  thrift::SparkNeighState sparkState{thrift::SparkNeighState::IDLE};

  /*
   * Timestamps for protocol timing.
   */
  std::chrono::steady_clock::time_point lastHelloSent;
  std::chrono::steady_clock::time_point lastHeartbeatSent;
  std::chrono::steady_clock::time_point lastHelloReceived;

  /*
   * Router-level configuration.
   */
  bool isOverloaded{false};
  int64_t nodeLabel{0};

  /*
   * Get the link-local IPv6 address for this router.
   * Used for Spark hello packets.
   */
  folly::IPAddressV6
  getLinkLocalAddr() const {
    return folly::IPAddressV6(
        fmt::format("fe80::{:x}", static_cast<uint16_t>(nodeId)));
  }

  /*
   * Get the transport address (for KvStore peering).
   */
  folly::IPAddressV6
  getTransportAddr() const {
    return folly::IPAddressV6(fmt::format("fc00::{:x}", nodeId));
  }

  /*
   * Increment and return the next hello sequence number.
   */
  uint64_t
  getNextHelloSeqNum() {
    return ++helloSeqNum;
  }

  /*
   * Increment and return the next heartbeat sequence number.
   */
  uint64_t
  getNextHeartbeatSeqNum() {
    return ++heartbeatSeqNum;
  }
};

/*
 * Topology definition containing all routers and their connections.
 * This is the top-level structure for defining a test topology.
 */
struct Topology {
  /*
   * All routers in the topology.
   * Key is the node name for fast lookup.
   */
  folly::F14FastMap<std::string, VirtualRouter> routers;

  /*
   * Ordered list of router names for deterministic iteration.
   */
  std::vector<std::string> routerNames;

  /*
   * Topology metadata.
   */
  std::string name;
  std::string description;

  /*
   * Statistics about the topology.
   */
  size_t
  getRouterCount() const {
    return routers.size();
  }

  size_t
  getTotalAdjacencyCount() const {
    size_t count = 0;
    for (const auto& [_, router] : routers) {
      count += router.adjacencies.size();
    }
    return count;
  }

  size_t
  getTotalPrefixCount() const {
    size_t count = 0;
    for (const auto& [_, router] : routers) {
      count += router.advertisedPrefixes.size();
    }
    return count;
  }

  /*
   * Get a router by name.
   * Throws if not found.
   */
  VirtualRouter&
  getRouter(const std::string& routerName) {
    auto it = routers.find(routerName);
    if (it == routers.end()) {
      throw std::runtime_error(fmt::format("Router not found: {}", routerName));
    }
    return it->second;
  }

  const VirtualRouter&
  getRouter(const std::string& routerName) const {
    auto it = routers.find(routerName);
    if (it == routers.end()) {
      throw std::runtime_error(fmt::format("Router not found: {}", routerName));
    }
    return it->second;
  }
};

} // namespace openr
