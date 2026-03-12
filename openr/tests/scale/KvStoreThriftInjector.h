/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <folly/io/async/EventBase.h>

#include <openr/if/gen-cpp2/OpenrCtrlAsyncClient.h>
#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

/*
 * KvStoreThriftInjector injects topology into a remote DUT via thrift.
 *
 * This is the "real network" counterpart to KvStoreBulkInjector.
 * Instead of pushing to an in-process queue, it calls the DUT's
 * OpenrCtrl thrift API (setKvStoreKeyVals).
 *
 * The topology conversion logic is the same as KvStoreBulkInjector:
 * - VirtualRouter -> thrift::AdjacencyDatabase
 * - VirtualRouter -> thrift::PrefixDatabase
 *
 * Usage:
 *   KvStoreThriftInjector injector("192.168.1.1", 2018);
 *   injector.connect();
 *   injector.injectTopology(topology);
 *
 *   // Query results
 *   auto adjKeys = injector.getKeys("adj:.*");
 */
class KvStoreThriftInjector {
 public:
  /*
   * Construct injector with DUT connection info.
   *
   * @param dutHost DUT hostname or IP address
   * @param dutPort DUT OpenR thrift port (default: 2018)
   */
  explicit KvStoreThriftInjector(
      const std::string& dutHost, uint16_t dutPort = 2018);

  ~KvStoreThriftInjector();

  /*
   * Connect to the DUT.
   * Must be called before any injection operations.
   *
   * @return true if connection successful
   */
  bool connect();

  /*
   * Disconnect from the DUT.
   */
  void disconnect();

  /*
   * Check if connected to the DUT.
   */
  bool isConnected() const;

  /*
   * Inject an entire topology into the DUT's KvStore.
   *
   * Creates AdjacencyDatabase and PrefixDatabase entries for all routers
   * and sends them to the DUT via thrift.
   *
   * @param topology The topology to inject
   * @param areaName OpenR area for the publications
   * @return Number of keys injected
   */
  size_t injectTopology(
      const Topology& topology, const std::string& areaName = "0");

  /*
   * Inject pre-built key-value pairs into the DUT's KvStore.
   *
   * @param keyVals The key-value pairs to inject
   * @param areaName OpenR area for the publications
   * @return Number of keys injected
   */
  size_t injectKeyVals(
      const thrift::KeyVals& keyVals, const std::string& areaName = "0");

  /*
   * Inject a single adjacency database update.
   *
   * @param router The virtual router
   * @param topology Full topology for building adjacencies
   * @param version Version number for the update
   * @param areaName OpenR area
   */
  void injectAdjacencyUpdate(
      const VirtualRouter& router,
      const Topology& topology,
      int64_t version,
      const std::string& areaName = "0");

  /*
   * Remove a node from the topology by injecting an empty adjacency database.
   *
   * @param nodeName Name of the node to remove
   * @param version Version number for the update
   * @param areaName OpenR area
   */
  void removeNode(
      const std::string& nodeName,
      int64_t version = 2,
      const std::string& areaName = "0");

  /*
   * Get keys from the DUT's KvStore matching a pattern.
   *
   * @param keyPrefix Prefix filter for keys (e.g., "adj:")
   * @param areaName OpenR area
   * @return Publication containing matching key-values
   */
  thrift::Publication getKeys(
      const std::string& keyPrefix, const std::string& areaName = "0");

  /*
   * Get all adjacency databases from the DUT.
   *
   * @param areaName OpenR area
   * @return Map of node name to adjacency database
   */
  std::map<std::string, thrift::AdjacencyDatabase> getAdjacencyDatabases(
      const std::string& areaName = "0");

  /*
   * Get route database from the DUT via Decision.
   *
   * @param nodeName Node to get routes for (typically the DUT's name)
   * @return Route database
   */
  thrift::RouteDatabase getRouteDatabase(const std::string& nodeName);

  /*
   * Get DUT's node name.
   *
   * @return DUT node name
   */
  std::string getDutNodeName();

  /*
   * Build thrift::KeyVals for a topology.
   * Public for use by KvStoreDataBuilder.
   *
   * @param topology The topology to build keys for
   * @param numFakeKeysPerNode Number of fake keys per node (0 = none)
   */
  static thrift::KeyVals buildKeyVals(
      const Topology& topology, int32_t numFakeKeysPerNode = 0);

  /*
   * Build thrift::AdjacencyDatabase from VirtualRouter.
   * Public for use by KvStoreDataBuilder.
   */
  static thrift::AdjacencyDatabase buildAdjacencyDatabase(
      const VirtualRouter& router, const Topology& topology);

  /*
   * Create a KvStore key-value pair for an adjacency database.
   * Public for use by KvStoreDataBuilder.
   */
  static std::pair<std::string, thrift::Value> createAdjKeyValue(
      const VirtualRouter& router,
      const Topology& topology,
      int64_t version = 1);

  /*
   * Create KvStore key-value pairs for prefix databases.
   * Public for use by KvStoreDataBuilder.
   */
  static std::vector<std::pair<std::string, thrift::Value>>
  createPrefixKeyValues(const VirtualRouter& router, int64_t version = 1);

  /*
   * Create KvStore key-value pairs for simulated fake keys.
   * Keys are named "fakekeys{i}:{nodeName}" where i = 0..numKeys-1.
   * Public for use by KvStoreDataBuilder.
   *
   * @param router The router to create fake keys for
   * @param numKeys Number of fake keys to create
   * @param version Version number for the keys
   */
  static std::vector<std::pair<std::string, thrift::Value>> createFakeKeyValues(
      const VirtualRouter& router, int32_t numKeys, int64_t version = 1);

 private:
  /*
   * DUT connection info
   */
  std::string dutHost_;
  uint16_t dutPort_;

  /*
   * Thrift client
   */
  std::unique_ptr<folly::EventBase> evb_;
  std::unique_ptr<apache::thrift::Client<thrift::OpenrCtrl>> client_;

  /*
   * Connection state
   */
  bool connected_{false};
};

} // namespace openr
