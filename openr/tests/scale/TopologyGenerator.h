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
 * BBF (Backbone Fabric) topology parameters.
 * 2-layer Clos with massive ECMP for backbone connectivity.
 */
constexpr int kDefaultBbfSpinesPerPlane = 32;
constexpr int kDefaultBbfLeavesPerPod = 16;
constexpr int kDefaultBbfEcmpWidth = 16; /* Links per spine-leaf pair */

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
   * Create a BBF (Backbone Fabric) 2-layer Clos topology.
   *
   * This models Meta's disaggregated backbone fabric with massive ECMP:
   * - Spine layer: High-radix switches at the top
   * - Leaf layer: Switches connecting to spine with multiple ECMP links
   *
   * Key characteristics:
   * - 2-layer Clos (Spine-Leaf)
   * - Configurable ECMP width (multiple parallel links per pair)
   * - Supports 300-4000+ nodes
   * - High fan-out for realistic backbone simulation
   *
   * Topology structure:
   *   Spine Layer:  [S0] [S1] [S2] ... [Sn]    (numPlanes * spinesPerPlane)
   *                   |    |    |        |
   *                   |    |    |        |     (ecmpWidth links each)
   *                   v    v    v        v
   *   Leaf Layer:   [L0] [L1] [L2] ... [Lm]    (numPods * leavesPerPod)
   *
   * @param numPods Number of leaf pods
   * @param numPlanes Number of spine planes
   * @param spinesPerPlane Spines per plane (default: 32)
   * @param leavesPerPod Leaves per pod (default: 16)
   * @param ecmpWidth Number of parallel links per spine-leaf pair (default: 16)
   * @param numPrefixesPerNode Prefixes per router
   * @return Topology with BBF connectivity
   */
  static Topology createBbf(
      int numPods,
      int numPlanes,
      int spinesPerPlane = kDefaultBbfSpinesPerPlane,
      int leavesPerPod = kDefaultBbfLeavesPerPod,
      int ecmpWidth = kDefaultBbfEcmpWidth,
      int numPrefixesPerNode = kDefaultPrefixesPerRouter);

  /*
   * Create a BBF topology with direct spine/leaf/control node counts.
   *
   * This is the simpler API that matches actual BBF hardware:
   * - numSpines: Total number of spine switches (e.g., 64)
   * - numLeaves: Total number of leaf switches (e.g., 252)
   * - numControlNodes: Number of control nodes running OpenR (e.g., 4)
   * - ecmpWidth: Number of parallel links per spine-leaf pair
   *
   * @return Topology with BBF connectivity
   */
  static Topology createBbfSimple(
      int numSpines,
      int numLeaves,
      int numControlNodes = 0,
      int ecmpWidth = kDefaultBbfEcmpWidth,
      int numPrefixesPerNode = kDefaultPrefixesPerRouter);

  /*
   * Calculate the total number of routers in a BBF topology.
   */
  static size_t calculateBbfRouterCount(
      int numPods, int numPlanes, int spinesPerPlane, int leavesPerPod);

  /*
   * Calculate the total number of adjacencies (links) in a BBF topology.
   * This includes ECMP links counted as separate adjacencies.
   */
  static size_t calculateBbfAdjacencyCount(
      int numPods,
      int numPlanes,
      int spinesPerPlane,
      int leavesPerPod,
      int ecmpWidth);

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
   * Generate a unique node name based on topology type.
   */
  static std::string getGridNodeName(int row, int col, int n);
  static std::string getFabricNodeName(uint8_t swMarker, int podId, int swId);
  static std::string getBbfSpineName(int planeId, int spineId);
  static std::string getBbfLeafName(int podId, int leafId);

  /*
   * Calculate the total number of routers in a fabric topology.
   */
  static size_t calculateFabricRouterCount(
      int numPods, int numPlanes, int numSswsPerPlane, int numRswsPerPod);

 private:
  /*
   * Helper to create adjacencies between two routers.
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

  /*
   * BBF topology helpers.
   */
  static void createBbfSpines(
      Topology& topo,
      int numPlanes,
      int spinesPerPlane,
      int numPrefixesPerNode,
      PrefixGenerator& prefixGen);

  static void createBbfLeaves(
      Topology& topo,
      int numPods,
      int leavesPerPod,
      int numPrefixesPerNode,
      PrefixGenerator& prefixGen);

  static void createBbfEcmpLinks(
      Topology& topo,
      int numPods,
      int numPlanes,
      int spinesPerPlane,
      int leavesPerPod,
      int ecmpWidth);

  /*
   * Helper to add ECMP adjacencies with unique interface names per link.
   */
  static void addEcmpAdjacencies(
      VirtualRouter& router1,
      VirtualRouter& router2,
      int ecmpWidth,
      int32_t metric = 1,
      int32_t latencyMs = 1);
};

} // namespace openr
