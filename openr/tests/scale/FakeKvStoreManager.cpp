/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/FakeKvStoreManager.h>

#include <fmt/format.h>
#include <glog/logging.h>

namespace openr {

FakeKvStoreManager::FakeKvStoreManager(uint16_t basePort, size_t ioThreads)
    : basePort_{basePort},
      nextPort_{basePort},
      ioPool_{std::make_shared<folly::IOThreadPoolExecutor>(ioThreads)} {
  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Created with basePort={}, ioThreads={}",
      basePort_,
      ioThreads);
}

FakeKvStoreManager::~FakeKvStoreManager() {
  if (running_) {
    stop();
  }
}

uint16_t
FakeKvStoreManager::addNeighbor(
    const std::string& neighborName, thrift::KeyVals kvStore) {
  if (running_) {
    throw std::runtime_error("Cannot add neighbors after servers have started");
  }

  if (servers_.find(neighborName) != servers_.end()) {
    throw std::runtime_error(
        fmt::format("Neighbor '{}' already exists", neighborName));
  }

  uint16_t port = nextPort_++;

  NeighborServer ns;
  ns.neighborName = neighborName;
  ns.port = port;
  ns.handler =
      std::make_shared<FakeKvStoreHandler>(neighborName, std::move(kvStore));

  ns.server = std::make_shared<apache::thrift::ThriftServer>();
  ns.server->setInterface(ns.handler);
  ns.server->setPort(port);
  ns.server->setIOThreadPool(ioPool_);
  ns.server->setNumCPUWorkerThreads(1);

  servers_.emplace(neighborName, std::move(ns));

  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Added neighbor '{}' on port {}", neighborName, port);

  return port;
}

void
FakeKvStoreManager::start() {
  if (running_) {
    LOG(WARNING) << "[FAKE-KVSTORE-MGR] Already running";
    return;
  }

  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Starting {} servers...", servers_.size());

  for (auto& [name, ns] : servers_) {
    ns.serverThread = std::make_unique<std::thread>([&ns]() {
      LOG(INFO) << fmt::format(
          "[FAKE-KVSTORE-MGR] Server for '{}' starting on port {}",
          ns.neighborName,
          ns.port);
      ns.server->serve();
      LOG(INFO) << fmt::format(
          "[FAKE-KVSTORE-MGR] Server for '{}' stopped", ns.neighborName);
    });
  }

  running_ = true;

  /*
   * Give servers time to bind.
   */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] All {} servers started (ports {}-{})",
      servers_.size(),
      basePort_,
      nextPort_ - 1);
}

void
FakeKvStoreManager::stop() {
  if (!running_) {
    return;
  }

  LOG(INFO) << "[FAKE-KVSTORE-MGR] Stopping all servers...";

  for (auto& [name, ns] : servers_) {
    if (ns.server) {
      ns.server->stop();
    }
  }

  for (auto& [name, ns] : servers_) {
    if (ns.serverThread && ns.serverThread->joinable()) {
      ns.serverThread->join();
    }
  }

  running_ = false;
  LOG(INFO) << "[FAKE-KVSTORE-MGR] All servers stopped";
}

uint16_t
FakeKvStoreManager::getPort(const std::string& neighborName) const {
  auto it = servers_.find(neighborName);
  if (it == servers_.end()) {
    throw std::runtime_error(
        fmt::format("Neighbor '{}' not found", neighborName));
  }
  return it->second.port;
}

void
FakeKvStoreManager::updateNeighborKvStore(
    const std::string& neighborName, thrift::KeyVals kvStore) {
  auto it = servers_.find(neighborName);
  if (it == servers_.end()) {
    throw std::runtime_error(
        fmt::format("Neighbor '{}' not found", neighborName));
  }
  it->second.handler->updateKvStore(std::move(kvStore));
}

std::shared_ptr<FakeKvStoreHandler>
FakeKvStoreManager::getHandler(const std::string& neighborName) {
  auto it = servers_.find(neighborName);
  if (it == servers_.end()) {
    return nullptr;
  }
  return it->second.handler;
}

void
FakeKvStoreManager::propagateKeyUpdate(
    const std::string& key, thrift::Value value) {
  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Propagating key '{}' (version={}) to {} neighbors",
      key,
      *value.version(),
      servers_.size());

  for (auto& [name, ns] : servers_) {
    ns.handler->updateKey(key, value);
  }
}

int64_t
FakeKvStoreManager::getNextVersion(const std::string& key) {
  auto it = keyVersions_.find(key);
  if (it == keyVersions_.end()) {
    keyVersions_[key] = 2;
    return 2;
  }
  return ++it->second;
}

void
FakeKvStoreManager::simulateLinkFlap(
    const std::string& routerName,
    const std::string& adjName,
    const Topology& topology,
    bool linkUp) {
  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Simulating link flap: {} <-> {} (linkUp={})",
      routerName,
      adjName,
      linkUp);

  const auto& router = topology.getRouter(routerName);
  int64_t version = getNextVersion(fmt::format("adj:{}", routerName));

  std::pair<std::string, thrift::Value> kv;
  if (linkUp) {
    kv = KvStoreDataBuilder::buildAdjKeyValue(router, topology, version);
  } else {
    kv = KvStoreDataBuilder::buildAdjKeyValueWithLinkDown(
        router, topology, adjName, version);
  }

  propagateKeyUpdate(kv.first, std::move(kv.second));
}

void
FakeKvStoreManager::simulateNodeRemoval(const std::string& routerName) {
  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Simulating node removal: {}", routerName);

  int64_t version = getNextVersion(fmt::format("adj:{}", routerName));
  auto [key, value] =
      KvStoreDataBuilder::buildRemovedNodeAdjKeyValue(routerName, version);

  propagateKeyUpdate(key, std::move(value));
}

void
FakeKvStoreManager::simulateNodeOverload(
    const std::string& routerName, const Topology& topology) {
  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Simulating node overload: {}", routerName);

  const auto& router = topology.getRouter(routerName);
  int64_t version = getNextVersion(fmt::format("adj:{}", routerName));
  auto [key, value] =
      KvStoreDataBuilder::buildOverloadedAdjKeyValue(router, topology, version);

  propagateKeyUpdate(key, std::move(value));
}

void
FakeKvStoreManager::updateTopology(
    const Topology& newTopology,
    const std::vector<std::string>& neighborNames) {
  LOG(INFO) << fmt::format(
      "[FAKE-KVSTORE-MGR] Updating topology for {} neighbors",
      neighborNames.size());

  auto allKvData =
      KvStoreDataBuilder::buildForAllNeighbors(neighborNames, newTopology);

  for (const auto& [name, kvData] : allKvData) {
    auto it = servers_.find(name);
    if (it != servers_.end()) {
      it->second.handler->updateKvStore(kvData);
    }
  }
}

std::vector<std::string>
FakeKvStoreManager::getNeighborNames() const {
  std::vector<std::string> names;
  names.reserve(servers_.size());
  for (const auto& [name, _] : servers_) {
    names.push_back(name);
  }
  return names;
}

} // namespace openr
