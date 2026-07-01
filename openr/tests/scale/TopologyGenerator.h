/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <openr/tests/mocks/PrefixGenerator.h>
#include <openr/tests/scale/VirtualRouter.h>

namespace openr {

/*
 * Fabric topology switch type markers.
 * Used to identify switch tiers in datacenter fabric topologies.
 */
constexpr uint8_t kScaleSswMarker = 1; // Spine Switch
constexpr uint8_t kScaleFswMarker = 2; // Fabric Switch
constexpr uint8_t kScaleRswMarker = 3; // Rack Switch

/*
 * Default fabric topology parameters matching Meta's datacenter design.
 */
constexpr int kDefaultSswsPerPlane = 36;
constexpr int kDefaultRswsPerPod = 48;
constexpr int kDefaultPrefixesPerRouter = 10;

/*
 * TopologyGenerator creates test topologies for OpenR scale testing.
 *
 * Supports multiple topology types:
 * 1. Grid: n×n mesh of routers (good for SPF stress testing)
 * 2. Fabric: Datacenter-style topology with SSW/FSW/RSW tiers
 * 3. Ring: Simple ring topology for basic testing
 * 4. Custom: Load from JSON file
 *
 * Usage:
 *   auto topo = TopologyGenerator::createGrid(10, 5); // 100 routers, 5
 * prefixes each auto fabric = TopologyGenerator::createFabric(8, 4); // 8 pods,
 * 4 planes auto custom = TopologyGenerator::loadFromFile("topology.json");
 */
class TopologyGenerator {
 public:
  TopologyGenerator() = default;

  /*
   * Create an n×n grid topology.
   *
   * Each router connects to its 4 neighbors (up, down, left, right).
   * Edge routers have fewer connections.
   *
   * @param n Grid dimension (creates n*n routers)
   * @param numPrefixesPerNode Number of prefixes each router advertises
   * @return Topology with grid connectivity
   */
  static Topology createGrid(int n, int numPrefixesPerNode = 1);

  /*
   * Create a datacenter fabric topology.
   *
   * Three-tier topology:
   * - SSW (Spine Switches): Top tier, each connects to one FSW per pod
   * - FSW (Fabric Switches): Middle tier, connects SSWs to RSWs
   * - RSW (Rack Switches): Bottom tier, connects to all FSWs in pod
   *
   * @param numPods Number of pods in the fabric
   * @param numPlanes Number of planes (equals number of FSWs per pod)
   * @param numSswsPerPlane SSWs per plane (default: 36)
   * @param numRswsPerPod RSWs per pod (default: 48)
   * @param numPrefixesPerNode Prefixes per router
   * @return Topology with fabric connectivity
   */
  static Topology createFabric(
      int numPods,
      int numPlanes,
      int numSswsPerPlane = kDefaultSswsPerPlane,
      int numRswsPerPod = kDefaultRswsPerPod,
      int numPrefixesPerNode = kDefaultPrefixesPerRouter);

  /*
   * Create a simple ring topology.
   *
   * Each router connects to its two neighbors in a ring.
   *
   * @param numRouters Number of routers in the ring
   * @param numPrefixesPerNode Prefixes per router
   * @return Topology with ring connectivity
   */
  static Topology createRing(int numRouters, int numPrefixesPerNode = 1);

  /*
   * Load topology from a JSON file.
   *
   * JSON format:
   * {
   *   "name": "topology_name",
   *   "routers": [
   *     {
   *       "name": "node-1",
   *       "adjacencies": [
   *         {"neighbor": "node-2", "interface": "eth0", "metric": 1}
   *       ],
   *       "prefixes": ["fc00:1::/64"]
   *     }
   *   ]
   * }
   *
   * @param filePath Path to the JSON topology file
   * @return Parsed topology
   */
  static Topology loadFromFile(const std::string& filePath);

  /*
   * Replicate a single-area topology into N self-consistent per-area copies.
   *
   * Each area gets a full copy of `base` whose node names are namespaced as
   * "<area>-<originalName>", whose routers are tagged with that area, and whose
   * node ids/labels are offset so they stay unique across copies. Adjacencies
   * and interface peers are remapped to the namespaced names, so every copy is
   * self-consistent and NO adjacency ever crosses an area boundary (the DUT,
   * spliced in later, is the only multi-area node). Prefixes are copied as-is,
   * so the same prefix is reachable in every area — a realistic ABR scenario.
   *
   * @param base Single-area topology to replicate
   * @param areaNames One area name per desired copy; must be non-empty and
   *     duplicate-free (throws std::invalid_argument otherwise)
   * @return Combined topology with base.getRouterCount() * N routers
   */
  static Topology replicateAcrossAreas(
      const Topology& base, const std::vector<std::string>& areaNames);

  /*
   * Generate a unique node name based on topology type.
   */
  static std::string getGridNodeName(int row, int col, int n);
  static std::string getFabricNodeName(uint8_t swMarker, int podId, int swId);

  /*
   * Calculate the total number of routers in a fabric topology.
   */
  static size_t calculateFabricRouterCount(
      int numPods, int numPlanes, int numSswsPerPlane, int numRswsPerPod);

  /*
   * Add bidirectional adjacency between two routers. Public so topology
   * generators in subdirectories (e.g. facebook/BbfTopologyGenerator) can
   * reuse it.
   */
  static void addBidirectionalAdjacency(
      VirtualRouter& router1,
      VirtualRouter& router2,
      const std::string& if1Name,
      const std::string& if2Name,
      int32_t metric = 1,
      int32_t latencyMs = 1);

  /*
   * Generate prefixes for a router.
   */
  static std::vector<thrift::PrefixEntry> generatePrefixes(
      const std::string& nodeName, int numPrefixes, PrefixGenerator& prefixGen);

  /*
   * Helper to get interface name between two routers.
   */
  static std::string getIfName(
      const std::string& localNode, const std::string& remoteNode);

 private:
  /*
   * Fabric topology helpers.
   */
  static void createFabricSsws(
      Topology& topo,
      int numPods,
      int numPlanes,
      int numSswsPerPlane,
      int numPrefixesPerNode,
      PrefixGenerator& prefixGen);

  static void createFabricFsws(
      Topology& topo,
      int numPods,
      int numPlanes,
      int numSswsPerPlane,
      int numRswsPerPod,
      int numPrefixesPerNode,
      PrefixGenerator& prefixGen);

  static void createFabricRsws(
      Topology& topo,
      int numPods,
      int numPlanes,
      int numSswsPerPlane,
      int numRswsPerPod,
      int numPrefixesPerNode,
      PrefixGenerator& prefixGen);
};

} // namespace openr
