/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/KvStoreThriftInjector.h>

#include <fmt/format.h>
#include <folly/io/async/AsyncSocket.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/LsdbUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

KvStoreThriftInjector::KvStoreThriftInjector(
    const std::string& dutHost, uint16_t dutPort)
    : dutHost_(dutHost), dutPort_(dutPort) {
  evb_ = std::make_unique<folly::EventBase>();
}

KvStoreThriftInjector::~KvStoreThriftInjector() {
  disconnect();
}

bool
KvStoreThriftInjector::connect() {
  if (connected_) {
    return true;
  }

  try {
    LOG(INFO) << fmt::format(
        "[KVSTORE-INJECTOR] Connecting to DUT at {}:{}...", dutHost_, dutPort_);

    auto socket = folly::AsyncSocket::newSocket(
        evb_.get(), dutHost_, dutPort_, 5000 /* connect timeout ms */);

    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(socket));

    client_ = std::make_unique<apache::thrift::Client<thrift::OpenrCtrl>>(
        std::move(channel));

    connected_ = true;
    LOG(INFO) << "[KVSTORE-INJECTOR] Connected to DUT successfully";
    return true;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to connect to DUT: {}", e.what());
    return false;
  }
}

void
KvStoreThriftInjector::disconnect() {
  if (client_) {
    client_.reset();
  }
  connected_ = false;
}

bool
KvStoreThriftInjector::isConnected() const {
  return connected_;
}

thrift::AdjacencyDatabase
KvStoreThriftInjector::buildAdjacencyDatabase(
    const VirtualRouter& router, const Topology& topology) {
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

  return createAdjDb(
      router.nodeName,
      adjs,
      static_cast<int32_t>(router.nodeLabel),
      router.isOverloaded,
      "0");
}

std::pair<std::string, thrift::Value>
KvStoreThriftInjector::createAdjKeyValue(
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
          0,
          0));
}

std::vector<std::pair<std::string, thrift::Value>>
KvStoreThriftInjector::createPrefixKeyValues(
    const VirtualRouter& router, int64_t version) {
  std::vector<std::pair<std::string, thrift::Value>> keyValues;
  apache::thrift::CompactSerializer serializer;

  for (const auto& prefixEntry : router.advertisedPrefixes) {
    auto [prefixKey, prefixDb] =
        createPrefixKeyAndDb(router.nodeName, prefixEntry, "0");

    keyValues.emplace_back(
        prefixKey.getPrefixKeyV2(),
        createThriftValue(
            version,
            router.nodeName,
            writeThriftObjStr(prefixDb, serializer),
            Constants::kTtlInfinity,
            0,
            0));
  }

  return keyValues;
}

std::vector<std::pair<std::string, thrift::Value>>
KvStoreThriftInjector::createFakeKeyValues(
    const VirtualRouter& router, int32_t numKeys, int64_t version) {
  std::vector<std::pair<std::string, thrift::Value>> keyValues;

  for (int32_t i = 0; i < numKeys; ++i) {
    std::string key = fmt::format("fakekeys{}:{}", i, router.nodeName);
    std::string payload = fmt::format("fake-data-{}-{}", router.nodeName, i);

    keyValues.emplace_back(
        std::move(key),
        createThriftValue(
            version,
            router.nodeName,
            std::move(payload),
            Constants::kTtlInfinity,
            0,
            0));
  }

  return keyValues;
}

thrift::KeyVals
KvStoreThriftInjector::buildKeyVals(
    const Topology& topology, int32_t numFakeKeysPerNode) {
  thrift::KeyVals keyVals;

  for (const auto& [nodeName, router] : topology.routers) {
    auto [key, value] = createAdjKeyValue(router, topology);
    keyVals.emplace(std::move(key), std::move(value));
  }

  for (const auto& [nodeName, router] : topology.routers) {
    auto prefixKvs = createPrefixKeyValues(router);
    for (auto& [key, value] : prefixKvs) {
      keyVals.emplace(std::move(key), std::move(value));
    }
  }

  if (numFakeKeysPerNode > 0) {
    for (const auto& [nodeName, router] : topology.routers) {
      auto fakeKvs = createFakeKeyValues(router, numFakeKeysPerNode);
      for (auto& [key, value] : fakeKvs) {
        keyVals.emplace(std::move(key), std::move(value));
      }
    }
  }

  return keyVals;
}

size_t
KvStoreThriftInjector::injectTopology(
    const Topology& topology, const std::string& areaName) {
  if (!connected_) {
    LOG(ERROR) << "[KVSTORE-INJECTOR] ERROR: Not connected to DUT";
    return 0;
  }

  LOG(INFO) << fmt::format(
      "[KVSTORE-INJECTOR] Injecting topology: {} routers, {} adjacencies, {} prefixes",
      topology.getRouterCount(),
      topology.getTotalAdjacencyCount(),
      topology.getTotalPrefixCount());

  auto keyVals = buildKeyVals(topology);
  size_t keyCount = keyVals.size();

  LOG(INFO) << fmt::format(
      "[KVSTORE-INJECTOR] Built {} key-value pairs, sending to area '{}'...",
      keyCount,
      areaName);

  thrift::KeySetParams params;
  params.keyVals() = std::move(keyVals);

  try {
    auto startTime = std::chrono::steady_clock::now();
    client_->sync_setKvStoreKeyVals(params, areaName);
    auto endTime = std::chrono::steady_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

    LOG(INFO) << fmt::format(
        "[KVSTORE-INJECTOR] SUCCESS: Injected {} keys in {} ms ({} keys/sec)",
        keyCount,
        durationMs,
        durationMs > 0 ? (keyCount * 1000 / durationMs) : keyCount);
    return keyCount;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to inject topology: {}", e.what());
    return 0;
  }
}

size_t
KvStoreThriftInjector::injectKeyVals(
    const thrift::KeyVals& keyVals, const std::string& areaName) {
  if (!connected_) {
    LOG(ERROR) << "[KVSTORE-INJECTOR] ERROR: Not connected to DUT";
    return 0;
  }

  size_t keyCount = keyVals.size();
  LOG(INFO) << fmt::format(
      "[KVSTORE-INJECTOR] Injecting {} pre-built key-value pairs to area '{}'...",
      keyCount,
      areaName);

  thrift::KeySetParams params;
  params.keyVals() = keyVals;

  try {
    auto startTime = std::chrono::steady_clock::now();
    client_->sync_setKvStoreKeyVals(params, areaName);
    auto endTime = std::chrono::steady_clock::now();
    auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

    LOG(INFO) << fmt::format(
        "[KVSTORE-INJECTOR] SUCCESS: Injected {} keys in {} ms ({} keys/sec)",
        keyCount,
        durationMs,
        durationMs > 0 ? (keyCount * 1000 / durationMs) : keyCount);
    return keyCount;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to inject key-values: {}", e.what());
    return 0;
  }
}

void
KvStoreThriftInjector::injectAdjacencyUpdate(
    const VirtualRouter& router,
    const Topology& topology,
    int64_t version,
    const std::string& areaName) {
  if (!connected_) {
    LOG(ERROR) << "[KVSTORE-INJECTOR] ERROR: Not connected to DUT";
    return;
  }

  auto [key, value] = createAdjKeyValue(router, topology, version);

  thrift::KeyVals keyVals;
  keyVals.emplace(std::move(key), std::move(value));

  thrift::KeySetParams params;
  params.keyVals() = std::move(keyVals);

  try {
    client_->sync_setKvStoreKeyVals(params, areaName);
    LOG(INFO) << fmt::format(
        "[KVSTORE-INJECTOR] Adjacency update: {} (version={})",
        router.nodeName,
        version);
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to inject adjacency update for {}: {}",
        router.nodeName,
        e.what());
  }
}

void
KvStoreThriftInjector::removeNode(
    const std::string& nodeName, int64_t version, const std::string& areaName) {
  if (!connected_) {
    LOG(ERROR) << "[KVSTORE-INJECTOR] ERROR: Not connected to DUT";
    return;
  }

  LOG(INFO) << fmt::format(
      "[KVSTORE-INJECTOR] Removing node {} (simulating failure)", nodeName);

  thrift::AdjacencyDatabase adjDb;
  adjDb.thisNodeName() = nodeName;
  adjDb.isOverloaded() = true;
  adjDb.adjacencies() = {};
  adjDb.nodeLabel() = 0;
  adjDb.area() = areaName;

  apache::thrift::CompactSerializer serializer;
  auto adjDbStr = writeThriftObjStr(adjDb, serializer);

  thrift::Value value;
  value.version() = version;
  value.originatorId() = nodeName;
  value.value() = std::move(adjDbStr);
  value.ttl() = 300000;
  value.ttlVersion() = 1;

  std::string key = fmt::format("adj:{}", nodeName);

  thrift::KeyVals keyVals;
  keyVals.emplace(std::move(key), std::move(value));

  thrift::KeySetParams params;
  params.keyVals() = std::move(keyVals);

  try {
    client_->sync_setKvStoreKeyVals(params, areaName);
    LOG(INFO)
        << fmt::format("[KVSTORE-INJECTOR] SUCCESS: Removed node {}", nodeName);
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to remove node {}: {}",
        nodeName,
        e.what());
  }
}

thrift::Publication
KvStoreThriftInjector::getKeys(
    const std::string& keyPrefix, const std::string& areaName) {
  if (!connected_) {
    LOG(ERROR) << "[KVSTORE-INJECTOR] ERROR: Not connected to DUT";
    return thrift::Publication{};
  }

  thrift::KeyDumpParams filter;
  filter.keys() = {keyPrefix};

  try {
    thrift::Publication pub;
    client_->sync_getKvStoreKeyValsFilteredArea(pub, filter, areaName);
    VLOG(1) << fmt::format(
        "[KVSTORE-INJECTOR] Got {} keys matching prefix '{}'",
        pub.keyVals()->size(),
        keyPrefix);
    return pub;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to get keys with prefix '{}': {}",
        keyPrefix,
        e.what());
    return thrift::Publication{};
  }
}

std::map<std::string, thrift::AdjacencyDatabase>
KvStoreThriftInjector::getAdjacencyDatabases(const std::string& areaName) {
  std::map<std::string, thrift::AdjacencyDatabase> result;

  auto pub = getKeys("adj:", areaName);

  apache::thrift::CompactSerializer serializer;
  for (const auto& [key, value] : *pub.keyVals()) {
    if (!value.value().has_value()) {
      continue;
    }

    try {
      auto adjDb = readThriftObjStr<thrift::AdjacencyDatabase>(
          value.value().value(), serializer);
      auto nodeName = *adjDb.thisNodeName();
      result.emplace(nodeName, std::move(adjDb));
    } catch (const std::exception& e) {
      LOG(WARNING) << "Failed to parse adjacency database for key " << key
                   << ": " << e.what();
    }
  }

  return result;
}

thrift::RouteDatabase
KvStoreThriftInjector::getRouteDatabase(const std::string& /* nodeName */) {
  if (!connected_) {
    LOG(ERROR) << "[KVSTORE-INJECTOR] ERROR: Not connected to DUT";
    return thrift::RouteDatabase{};
  }

  try {
    thrift::RouteDatabase routeDb;
    client_->sync_getRouteDb(routeDb);
    LOG(INFO) << fmt::format(
        "[KVSTORE-INJECTOR] Route database: {} unicast routes, {} MPLS routes",
        routeDb.unicastRoutes()->size(),
        routeDb.mplsRoutes()->size());
    return routeDb;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to get route database: {}", e.what());
    return thrift::RouteDatabase{};
  }
}

std::string
KvStoreThriftInjector::getDutNodeName() {
  if (!connected_) {
    LOG(ERROR) << "[KVSTORE-INJECTOR] ERROR: Not connected to DUT";
    return "";
  }

  try {
    std::string nodeName;
    client_->sync_getMyNodeName(nodeName);
    LOG(INFO) << fmt::format("[KVSTORE-INJECTOR] DUT node name: {}", nodeName);
    return nodeName;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[KVSTORE-INJECTOR] ERROR: Failed to get DUT node name: {}", e.what());
    return "";
  }
}

} // namespace openr
