/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/container/F14Map.h>

#include <openr/kvstore/tests/utils/TestUtil.h>

namespace openr {
namespace util {
using namespace ::openr;

std::string
genNodeName(size_t i) {
  return folly::to<std::string>("node-", i);
}

void
generateTopo(
    const std::vector<std::unique_ptr<
        KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>>>& stores,
    ClusterTopology topo) {
  switch (topo) {
    /*
     * Linear Topology Illustration:
     * 0 - 1 - 2 - 3 - 4 - 5 - 6 - 7
     */
  case ClusterTopology::LINEAR: {
    if (stores.empty()) {
      // no peers to connect
      return;
    }
    KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* prev =
        stores.front().get();
    for (size_t i = 1; i < stores.size(); i++) {
      KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* cur =
          stores.at(i).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
      prev = cur;
    }
    break;
  }
  /*
   * Ring Topology Illustration:
   *   1 - 3 - 5
   *  /         \
   * 0           7
   *  \         /
   *   2 - 4 - 6
   * This is designed such that the last node is the furthest from first node
   */
  case ClusterTopology::RING: {
    for (size_t i = 1; i < stores.size(); i++) {
      KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* cur =
          stores.at(i).get();
      KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* prev =
          stores.at(i == 1 ? 0 : i - 2).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
    }
    if (stores.size() > 2) {
      KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* cur =
          stores.back().get();
      KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* prev =
          stores.at(stores.size() - 2).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
    }
    break;
  }
  /*
   * Star Topology Illustration:
   *    1   2
   *     \ /
   *  6 - 0 - 3
   *     / \
   *    5   4
   * Every additional node is directly connected to center
   */
  case ClusterTopology::STAR: {
    for (size_t i = 1; i < stores.size(); i++) {
      KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* center =
          stores.front().get();
      KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* cur =
          stores.at(i).get();
      center->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(
          kTestingAreaName, center->getNodeId(), center->getPeerSpec());
    }
    break;
  }
  default: {
    throw std::runtime_error("invalid topology type");
  }
  }
}

folly::coro::Task<void>
co_validateNodeKey(
    const folly::F14FastMap<std::string, ::openr::thrift::Value>& events,
    ::openr::KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>* node) {
  while (events.size() != node->dumpAll(kTestingAreaName).size()) {
    // yield to avoid hogging the process
    std::this_thread::yield();
  }
  co_return;
}

folly::coro::Task<void>
co_waitForConvergence(
    const folly::F14FastMap<std::string, ::openr::thrift::Value>& events,
    const std::vector<std::unique_ptr<
        ::openr::KvStoreWrapper<::openr::thrift::KvStoreServiceAsyncClient>>>&
        stores) {
  co_await folly::coro::collectAllWindowed(
      [&]() -> folly::coro::Generator<folly::coro::Task<void>&&> {
        for (size_t i = 0; i < stores.size(); i++) {
          co_yield co_validateNodeKey(events, stores.at(i).get());
        }
      }(),
      10);
}

} // namespace util
} // namespace openr
