/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/KvStoreBulkInjector.h>

#include <fmt/format.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/Util.h>

namespace openr {

KvStoreBulkInjector::KvStoreBulkInjector(
    messaging::ReplicateQueue<KvStorePublication>& kvStoreQueue)
    : kvStoreQueue_(kvStoreQueue) {}

std::vector<thrift::Adjacency>
KvStoreBulkInjector::buildThriftAdjacencies(
    const VirtualRouter& router, const Topology& topology) {
  std::vector<thrift::Adjacency> adjs;

  for (const auto& adj : router.adjacencies) {
    if (!adj.isUp) {
      continue; // Skip down adjacencies
    }

    /*
     * Get the neighbor router to fetch its address info
     */
    const auto& neighbor = topology.getRouter(adj.remoteRouterName);

    adjs.push_back(createThriftAdjacency(
        adj.remoteRouterName,
        adj.localIfName,
        fmt::format("fe80::{:x}", static_cast<uint16_t>(neighbor.nodeId)),
        fmt::format(
            "10.0.{}.{}",
            (neighbor.nodeId >> 8) & 0xff,
            neighbor.nodeId & 0xff),
        adj.metric,
        static_cast<int32_t>(neighbor.nodeLabel),
        false /* overload */,
        adj.metric * 100 /* rtt */,
        10000 /* timestamp */,
        1 /* weight */,
        adj.remoteIfName));
  }

  return adjs;
}

thrift::AdjacencyDatabase
KvStoreBulkInjector::buildAdjacencyDatabase(
    const VirtualRouter& router, const Topology& topology) {
  auto adjs = buildThriftAdjacencies(router, topology);
  return createAdjDb(
      router.nodeName,
      adjs,
      static_cast<int32_t>(router.nodeLabel),
      router.isOverloaded);
}

std::pair<std::string, thrift::Value>
KvStoreBulkInjector::createAdjKeyValue(
    const VirtualRouter& router, const Topology& topology, int64_t version) {
  auto adjDb = buildAdjacencyDatabase(router, topology);

  apache::thrift::CompactSerializer serializer;
  std::string key = fmt::format("adj:{}", router.nodeName);

  return std::make_pair(
      key,
      createThriftValue(
          version,
          router.nodeName,
          writeThriftObjStr(adjDb, serializer),
          Constants::kTtlInfinity,
          0 /* ttlVersion */,
          0 /* hash */));
}

std::vector<std::pair<std::string, thrift::Value>>
KvStoreBulkInjector::createPrefixKeyValues(
    const VirtualRouter& router, int64_t version) {
  std::vector<std::pair<std::string, thrift::Value>> keyValues;
  apache::thrift::CompactSerializer serializer;

  for (const auto& prefixEntry : router.advertisedPrefixes) {
    auto [prefixKey, prefixDb] =
        createPrefixKeyAndDb(router.nodeName, prefixEntry);

    keyValues.emplace_back(
        prefixKey.getPrefixKeyV2(),
        createThriftValue(
            version,
            router.nodeName,
            writeThriftObjStr(prefixDb, serializer),
            Constants::kTtlInfinity,
            0 /* ttlVersion */,
            0 /* hash */));
  }

  return keyValues;
}

void
KvStoreBulkInjector::injectTopology(
    const Topology& topology, const std::string& areaName) {
  LOG(INFO) << fmt::format(
      "Injecting topology with {} routers, {} adjacencies, {} prefixes",
      topology.getRouterCount(),
      topology.getTotalAdjacencyCount(),
      topology.getTotalPrefixCount());

  thrift::Publication pub;
  pub.area() = areaName;
  thrift::KeyVals keyVals;

  /*
   * Add all adjacency databases
   */
  for (const auto& [nodeName, router] : topology.routers) {
    auto [key, value] = createAdjKeyValue(router, topology);
    keyVals.emplace(std::move(key), std::move(value));
  }

  /*
   * Add all prefix databases
   */
  for (const auto& [nodeName, router] : topology.routers) {
    auto prefixKvs = createPrefixKeyValues(router);
    for (auto& [key, value] : prefixKvs) {
      keyVals.emplace(std::move(key), std::move(value));
    }
  }

  pub.keyVals() = std::move(keyVals);

  LOG(INFO) << fmt::format(
      "Publishing {} key-value pairs to KvStore", pub.keyVals()->size());

  kvStoreQueue_.push(std::move(pub));
}

void
KvStoreBulkInjector::injectAdjacencyDb(
    const VirtualRouter& /* router */, const std::string& /* areaName */) {
  /*
   * We need the full topology to build adjacencies, but for single-router
   * injection we create a minimal topology with just this router.
   */
  LOG(WARNING) << "injectAdjacencyDb requires full topology context. "
               << "Use injectTopology or injectAdjacencyUpdate instead.";
}

void
KvStoreBulkInjector::injectPrefixDb(
    const VirtualRouter& router, const std::string& areaName) {
  thrift::Publication pub;
  pub.area() = areaName;
  thrift::KeyVals keyVals;

  auto prefixKvs = createPrefixKeyValues(router);
  for (auto& [key, value] : prefixKvs) {
    keyVals.emplace(std::move(key), std::move(value));
  }

  pub.keyVals() = std::move(keyVals);
  kvStoreQueue_.push(std::move(pub));
}

void
KvStoreBulkInjector::sendKvStoreSyncedEvent() {
  LOG(INFO) << "Sending KvStore synced event to trigger route computation";
  kvStoreQueue_.push(thrift::InitializationEvent::KVSTORE_SYNCED);
}

void
KvStoreBulkInjector::injectAdjacencyUpdate(
    const VirtualRouter& /* router */,
    int64_t /* version */,
    const std::string& /* areaName */) {
  /*
   * For updates, we need to rebuild with the topology context.
   * This is typically called from simulateLinkFlap or simulateOverload.
   */
  LOG(WARNING) << "injectAdjacencyUpdate requires topology context. "
               << "Use simulateLinkFlap or simulateOverload instead.";
}

void
KvStoreBulkInjector::injectPrefixUpdate(
    const VirtualRouter& router, int64_t version, const std::string& areaName) {
  thrift::Publication pub;
  pub.area() = areaName;
  thrift::KeyVals keyVals;

  auto prefixKvs = createPrefixKeyValues(router, version);
  for (auto& [key, value] : prefixKvs) {
    keyVals.emplace(std::move(key), std::move(value));
  }

  pub.keyVals() = std::move(keyVals);
  kvStoreQueue_.push(std::move(pub));
}

void
KvStoreBulkInjector::simulateLinkFlap(
    Topology& topology,
    const std::string& routerName,
    const std::string& ifName,
    bool isUp,
    int64_t version) {
  auto& router = topology.getRouter(routerName);

  /*
   * Find and update the adjacency state
   */
  bool found = false;
  for (auto& adj : router.adjacencies) {
    if (adj.localIfName == ifName) {
      adj.isUp = isUp;
      found = true;
      LOG(INFO) << fmt::format(
          "Simulating link {} on {}:{} (now {})",
          isUp ? "up" : "down",
          routerName,
          ifName,
          isUp ? "UP" : "DOWN");
      break;
    }
  }

  if (!found) {
    LOG(WARNING) << fmt::format(
        "No adjacency found for interface {} on router {}. Link flap not simulated.",
        ifName,
        routerName);
    return;
  }

  /*
   * Also update the interface state
   */
  for (auto& iface : router.interfaces) {
    if (iface.ifName == ifName) {
      iface.isUp = isUp;
      break;
    }
  }

  /*
   * Publish the updated adjacency database
   */
  thrift::Publication pub;
  pub.area() = std::string(kScaleTestAreaName);
  thrift::KeyVals keyVals;

  auto [key, value] = createAdjKeyValue(router, topology, version);
  keyVals.emplace(std::move(key), std::move(value));

  pub.keyVals() = std::move(keyVals);
  kvStoreQueue_.push(std::move(pub));
}

void
KvStoreBulkInjector::simulateOverload(
    Topology& topology,
    const std::string& routerName,
    bool isOverloaded,
    int64_t version) {
  auto& router = topology.getRouter(routerName);
  router.isOverloaded = isOverloaded;

  LOG(INFO) << fmt::format(
      "Simulating {} overload on {}",
      isOverloaded ? "set" : "clear",
      routerName);

  /*
   * Publish the updated adjacency database with overload bit
   */
  thrift::Publication pub;
  pub.area() = std::string(kScaleTestAreaName);
  thrift::KeyVals keyVals;

  auto [key, value] = createAdjKeyValue(router, topology, version);
  keyVals.emplace(std::move(key), std::move(value));

  pub.keyVals() = std::move(keyVals);
  kvStoreQueue_.push(std::move(pub));
}

void
KvStoreBulkInjector::removeNode(const std::string& nodeName, int64_t version) {
  LOG(INFO) << fmt::format(
      "Removing node {} from topology (simulating failure)", nodeName);

  /*
   * Create an empty adjacency database for this node.
   * This signals to Decision that the node has no adjacencies.
   */
  thrift::AdjacencyDatabase adjDb;
  adjDb.thisNodeName() = nodeName;
  adjDb.isOverloaded() = true; /* Mark as overloaded so it's not used */
  adjDb.adjacencies() = {}; /* No adjacencies */
  adjDb.nodeLabel() = 0;
  adjDb.area() = std::string(kScaleTestAreaName);

  /*
   * Serialize and create KvStore value
   */
  apache::thrift::CompactSerializer serializer;
  auto adjDbStr = writeThriftObjStr(adjDb, serializer);

  thrift::Value value;
  value.version() = version;
  value.originatorId() = nodeName;
  value.value() = std::move(adjDbStr);
  value.ttl() = 300000; /* 5 minutes */
  value.ttlVersion() = 1;

  std::string key = fmt::format("adj:{}", nodeName);

  /*
   * Publish the update
   */
  thrift::Publication pub;
  pub.area() = std::string(kScaleTestAreaName);
  thrift::KeyVals keyVals;
  keyVals.emplace(std::move(key), std::move(value));
  pub.keyVals() = std::move(keyVals);

  kvStoreQueue_.push(std::move(pub));
}

} // namespace openr
