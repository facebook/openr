/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/tests/scale/FakeKvStoreHandler.h>
#include <openr/tests/scale/KvStoreDataBuilder.h>
#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

/*
 * FakeKvStoreManager manages per-neighbor Thrift servers for scale testing.
 *
 * Each fake neighbor gets its own lightweight Thrift server on a unique port.
 * When SparkFaker establishes an adjacency, the DUT's KvStore will connect
 * to the neighbor's port (advertised in the Spark handshake) and perform
 * a 3-way full-sync.
 *
 * The manager provides APIs for:
 * - Adding neighbors with their KV data
 * - Starting/stopping all servers
 * - Topology change propagation (simulating flooding)
 *
 * Threading:
 * - Shared IOThreadPoolExecutor for all 64 servers (efficient IO)
 * - One thread per server for serve() loop (could optimize later)
 *
 * Usage:
 *   FakeKvStoreManager manager(3000, 4);  // basePort=3000, 4 IO threads
 *
 *   auto kvData = KvStoreDataBuilder::buildForAllNeighbors(neighbors,
 * topology); for (const auto& [name, data] : kvData) { uint16_t port =
 * manager.addNeighbor(name, std::move(data));
 *     sparkFaker->setNeighborCtrlPort(name, port);
 *   }
 *
 *   manager.start();
 *   // ... run test ...
 *   manager.stop();
 */
class FakeKvStoreManager {
 public:
  /*
   * Construct the manager.
   *
   * @param basePort First port number to assign (e.g., 3000)
   * @param ioThreads Number of IO threads in shared pool (default: 4)
   */
  explicit FakeKvStoreManager(uint16_t basePort, size_t ioThreads = 4);

  ~FakeKvStoreManager();

  /*
   * Add a neighbor with its KV data.
   *
   * @param neighborName Name of the fake neighbor
   * @param kvStore Initial KV data for this neighbor
   * @return Port assigned to this neighbor
   */
  uint16_t addNeighbor(
      const std::string& neighborName, thrift::KeyVals kvStore);

  /*
   * Add a neighbor sharing immutable KV data (COW path).
   *
   * Multiple neighbors can share the same underlying data.
   * Each handler materializes a private copy on first write.
   *
   * @param neighborName Name of the fake neighbor
   * @param sharedKvStore Shared immutable KV data
   * @return Port assigned to this neighbor
   */
  uint16_t addNeighbor(
      const std::string& neighborName,
      std::shared_ptr<const thrift::KeyVals> sharedKvStore);

  /*
   * Start all Thrift servers.
   * Must be called after all neighbors are added.
   */
  void start();

  /*
   * Stop all Thrift servers.
   */
  void stop();

  /*
   * Check if servers are running.
   */
  bool
  isRunning() const {
    return running_;
  }

  /*
   * Get the port assigned to a neighbor.
   * Throws if neighbor not found.
   */
  uint16_t getPort(const std::string& neighborName) const;

  /*
   * Update a specific neighbor's KV data.
   */
  void updateNeighborKvStore(
      const std::string& neighborName, thrift::KeyVals kvStore);

  /*
   * Get a handler for direct manipulation (for testing).
   */
  std::shared_ptr<FakeKvStoreHandler> getHandler(
      const std::string& neighborName);

  /*
   * Propagate a key-value update to ALL neighbor handlers.
   * Simulates flooding convergence — every peer gets the update.
   * The version in the provided value is used directly.
   */
  void propagateKeyUpdate(const std::string& key, thrift::Value value);

  /*
   * Simulate a link flap: rebuild the adj DB for the affected router
   * with the adjacency removed/restored, then propagate to all neighbors.
   *
   * @param routerName The router whose link flapped
   * @param adjName The neighbor that became unreachable/reachable
   * @param topology Current topology (for rebuilding adj DB)
   * @param linkUp true = link came up, false = link went down
   */
  void simulateLinkFlap(
      const std::string& routerName,
      const std::string& adjName,
      const Topology& topology,
      bool linkUp);

  /*
   * Simulate node removal: inject an overloaded, empty adj DB
   * for the node, propagated to all neighbors.
   */
  void simulateNodeRemoval(const std::string& routerName);

  /*
   * Simulate node overload: set the overload bit on the router's
   * adj DB and propagate to all neighbors.
   */
  void simulateNodeOverload(
      const std::string& routerName, const Topology& topology);

  /*
   * Full topology update: rebuild all per-neighbor KV data
   * from a new topology and push to all handlers.
   *
   * @param newTopology New topology to use
   * @param neighborNames List of neighbors to update
   * @param numFakeKeysPerNode Number of fake keys per node (0 = none)
   */
  void updateTopology(
      const Topology& newTopology,
      const std::vector<std::string>& neighborNames,
      int32_t numFakeKeysPerNode = 0);

  /*
   * Get list of all neighbor names.
   */
  std::vector<std::string> getNeighborNames() const;

  /*
   * Get the number of neighbors.
   */
  size_t
  getNeighborCount() const {
    return servers_.size();
  }

 private:
  struct NeighborServer {
    std::string neighborName;
    uint16_t port{};
    std::shared_ptr<FakeKvStoreHandler> handler;
    std::shared_ptr<apache::thrift::ThriftServer> server;
    std::unique_ptr<std::thread> serverThread;
  };

  uint16_t basePort_;
  uint16_t nextPort_;
  bool running_{false};

  std::map<std::string, int64_t> keyVersions_;

  std::shared_ptr<folly::IOThreadPoolExecutor> ioPool_;

  std::map<std::string, NeighborServer> servers_;

  int64_t getNextVersion(const std::string& key);
};

} // namespace openr
