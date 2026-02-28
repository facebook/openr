/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

/*
 * Area name used for scale testing.
 */
constexpr folly::StringPiece kScaleTestAreaName = "area0";

/*
 * KvStoreBulkInjector injects topology directly into KvStore.
 *
 * This bypasses the Spark protocol entirely, allowing efficient testing
 * of KvStore flooding and Decision SPF computation at scale.
 *
 * The injector converts VirtualRouter topology into KvStore publications:
 * - AdjacencyDatabase entries for each router
 * - PrefixDatabase entries for each router's prefixes
 *
 * Usage:
 *   KvStoreBulkInjector injector(kvStoreQueue);
 *   injector.injectTopology(topology);
 *   injector.sendKvStoreSyncedEvent(); // Trigger route computation
 */
class KvStoreBulkInjector {
 public:
  /*
   * Construct injector with reference to KvStore publication queue.
   *
   * @param kvStoreQueue Queue to push publications to
   */
  explicit KvStoreBulkInjector(
      messaging::ReplicateQueue<KvStorePublication>& kvStoreQueue);

  /*
   * Inject an entire topology into KvStore.
   *
   * Creates AdjacencyDatabase and PrefixDatabase entries for all routers
   * and publishes them to KvStore in a single batch.
   *
   * @param topology The topology to inject
   * @param areaName OpenR area for the publications
   */
  void injectTopology(
      const Topology& topology,
      const std::string& areaName = std::string(kScaleTestAreaName));

  /*
   * Inject a single router's adjacency database.
   *
   * @param router The virtual router
   * @param areaName OpenR area
   */
  void injectAdjacencyDb(
      const VirtualRouter& router,
      const std::string& areaName = std::string(kScaleTestAreaName));

  /*
   * Inject a single router's prefix database.
   *
   * @param router The virtual router
   * @param areaName OpenR area
   */
  void injectPrefixDb(
      const VirtualRouter& router,
      const std::string& areaName = std::string(kScaleTestAreaName));

  /*
   * Send KvStore synced event to trigger route computation.
   *
   * Call this after injecting the initial topology to trigger
   * Decision's initial route calculation.
   */
  void sendKvStoreSyncedEvent();

  /*
   * Inject an adjacency update (e.g., link flap, overload toggle).
   *
   * @param router Router with updated adjacencies
   * @param version Version number for the update (should be > previous)
   * @param areaName OpenR area
   */
  void injectAdjacencyUpdate(
      const VirtualRouter& router,
      int64_t version,
      const std::string& areaName = std::string(kScaleTestAreaName));

  /*
   * Inject a prefix update.
   *
   * @param router Router with updated prefixes
   * @param version Version number
   * @param areaName OpenR area
   */
  void injectPrefixUpdate(
      const VirtualRouter& router,
      int64_t version,
      const std::string& areaName = std::string(kScaleTestAreaName));

  /*
   * Simulate a link flap by toggling adjacency state.
   *
   * @param topology The full topology
   * @param routerName Router experiencing the flap
   * @param ifName Interface that flapped
   * @param isUp New state of the link
   * @param version Version number for the update
   */
  void simulateLinkFlap(
      Topology& topology,
      const std::string& routerName,
      const std::string& ifName,
      bool isUp,
      int64_t version);

  /*
   * Simulate router overload by setting overload bit.
   *
   * @param topology The full topology
   * @param routerName Router to overload
   * @param isOverloaded New overload state
   * @param version Version number for the update
   */
  void simulateOverload(
      Topology& topology,
      const std::string& routerName,
      bool isOverloaded,
      int64_t version);

  /*
   * Remove a node from the topology by injecting an empty adjacency database.
   *
   * This simulates a node going completely down - all its adjacencies
   * disappear.
   *
   * @param nodeName Name of the node to remove
   * @param version Version number for the update (default: 2)
   */
  void removeNode(const std::string& nodeName, int64_t version = 2);

 private:
  /*
   * Convert VirtualRouter adjacencies to thrift Adjacency list.
   */
  std::vector<thrift::Adjacency> buildThriftAdjacencies(
      const VirtualRouter& router, const Topology& topology);

  /*
   * Build thrift AdjacencyDatabase from VirtualRouter.
   */
  thrift::AdjacencyDatabase buildAdjacencyDatabase(
      const VirtualRouter& router, const Topology& topology);

  /*
   * Create a KvStore key-value pair for an adjacency database.
   */
  std::pair<std::string, thrift::Value> createAdjKeyValue(
      const VirtualRouter& router,
      const Topology& topology,
      int64_t version = 1);

  /*
   * Create KvStore key-value pairs for prefix databases.
   */
  std::vector<std::pair<std::string, thrift::Value>> createPrefixKeyValues(
      const VirtualRouter& router, int64_t version = 1);

  /*
   * Reference to the KvStore publication queue.
   */
  messaging::ReplicateQueue<KvStorePublication>& kvStoreQueue_;
};

} // namespace openr
