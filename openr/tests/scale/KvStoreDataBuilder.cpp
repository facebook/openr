/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/KvStoreDataBuilder.h>

#include <fmt/format.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/Util.h>
#include <openr/tests/scale/KvStoreThriftInjector.h>

namespace openr {

thrift::KeyVals
KvStoreDataBuilder::buildForNeighbor(const Topology& topology) {
  thrift::KeyVals keyVals;

  for (const auto& [nodeName, router] : topology.routers) {
    auto [adjKey, adjValue] =
        KvStoreThriftInjector::createAdjKeyValue(router, topology);
    keyVals.emplace(std::move(adjKey), std::move(adjValue));

    auto prefixKvs = KvStoreThriftInjector::createPrefixKeyValues(router);
    for (auto& [prefixKey, prefixValue] : prefixKvs) {
      keyVals.emplace(std::move(prefixKey), std::move(prefixValue));
    }
  }

  return keyVals;
}

std::map<std::string, thrift::KeyVals>
KvStoreDataBuilder::buildForAllNeighbors(
    const std::vector<std::string>& neighborNames, const Topology& topology) {
  /*
   * Build shared topology data once. Since each neighbor sees the full
   * topology (after flooding converges), we can reuse this for all neighbors.
   *
   * The only difference per neighbor is that each neighbor's own adj/prefix
   * keys have that neighbor as originatorId. But since we're simulating
   * flooded data (not self-originated), all keys use their actual router's
   * nodeName as originatorId.
   */
  thrift::KeyVals sharedKeyVals = KvStoreThriftInjector::buildKeyVals(topology);

  std::map<std::string, thrift::KeyVals> result;
  for (const auto& neighborName : neighborNames) {
    /*
     * Each neighbor gets a copy of the shared topology.
     * In the future, we could add per-neighbor variations here
     * (e.g., missing keys, version skew, stale entries).
     */
    result.emplace(neighborName, sharedKeyVals);
  }

  return result;
}

std::pair<std::string, thrift::Value>
KvStoreDataBuilder::buildAdjKeyValue(
    const VirtualRouter& router, const Topology& topology, int64_t version) {
  return KvStoreThriftInjector::createAdjKeyValue(router, topology, version);
}

std::vector<std::pair<std::string, thrift::Value>>
KvStoreDataBuilder::buildPrefixKeyValues(
    const VirtualRouter& router, int64_t version) {
  return KvStoreThriftInjector::createPrefixKeyValues(router, version);
}

std::pair<std::string, thrift::Value>
KvStoreDataBuilder::buildAdjKeyValueWithLinkDown(
    const VirtualRouter& router,
    const Topology& topology,
    const std::string& adjToRemove,
    int64_t version) {
  /*
   * Build adjacency database with one adjacency filtered out.
   */
  std::vector<thrift::Adjacency> adjs;

  for (const auto& adj : router.adjacencies) {
    if (!adj.isUp || adj.remoteRouterName == adjToRemove) {
      continue;
    }

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
        false,
        adj.metric * 100,
        10000,
        1,
        adj.remoteIfName));
  }

  auto adjDb = createAdjDb(
      router.nodeName,
      adjs,
      static_cast<int32_t>(router.nodeLabel),
      router.isOverloaded);

  apache::thrift::CompactSerializer serializer;
  std::string key = fmt::format("adj:{}", router.nodeName);

  return std::make_pair(
      key,
      createThriftValue(
          version,
          router.nodeName,
          writeThriftObjStr(adjDb, serializer),
          Constants::kTtlInfinity,
          0,
          0));
}

std::pair<std::string, thrift::Value>
KvStoreDataBuilder::buildRemovedNodeAdjKeyValue(
    const std::string& routerName, int64_t version) {
  /*
   * Create an empty, overloaded adjacency database.
   * This signals to other routers that this node is down.
   */
  thrift::AdjacencyDatabase adjDb;
  adjDb.thisNodeName() = routerName;
  adjDb.isOverloaded() = true;
  adjDb.adjacencies() = {};
  adjDb.nodeLabel() = 0;
  adjDb.area() = "area0";

  apache::thrift::CompactSerializer serializer;
  std::string key = fmt::format("adj:{}", routerName);

  thrift::Value value;
  value.version() = version;
  value.originatorId() = routerName;
  value.value() = writeThriftObjStr(adjDb, serializer);
  value.ttl() = 300000;
  value.ttlVersion() = 1;

  return std::make_pair(std::move(key), std::move(value));
}

std::pair<std::string, thrift::Value>
KvStoreDataBuilder::buildOverloadedAdjKeyValue(
    const VirtualRouter& router, const Topology& topology, int64_t version) {
  /*
   * Build adjacency database with overload bit set.
   * Node keeps its adjacencies but won't be used for transit.
   */
  std::vector<thrift::Adjacency> adjs;

  for (const auto& adj : router.adjacencies) {
    if (!adj.isUp) {
      continue;
    }

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
        false,
        adj.metric * 100,
        10000,
        1,
        adj.remoteIfName));
  }

  auto adjDb = createAdjDb(
      router.nodeName, adjs, static_cast<int32_t>(router.nodeLabel), true);

  apache::thrift::CompactSerializer serializer;
  std::string key = fmt::format("adj:{}", router.nodeName);

  return std::make_pair(
      key,
      createThriftValue(
          version,
          router.nodeName,
          writeThriftObjStr(adjDb, serializer),
          Constants::kTtlInfinity,
          0,
          0));
}

} // namespace openr
