/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
    if (stores.size() <= 1) {
      // no peers to connect
      return;
    }
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
  default: {
    throw std::runtime_error("invalid topology type");
  }
  }
}

void
validateLastNodeKey(
    size_t numExpectedKey,
    KvStoreWrapper<thrift::KvStoreServiceAsyncClient>* node) {
  while (numExpectedKey != node->dumpAll(kTestingAreaName).size()) {
    // yield to avoid hogging the process
    std::this_thread::yield();
  }
}

} // namespace util
} // namespace openr
