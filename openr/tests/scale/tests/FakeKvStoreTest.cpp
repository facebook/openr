/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <fmt/format.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/tests/scale/FakeKvStoreHandler.h>
#include <openr/tests/scale/FakeKvStoreManager.h>
#include <openr/tests/scale/KvStoreDataBuilder.h>
#include <openr/tests/scale/TopologyGenerator.h>

namespace openr {

class FakeKvStoreTest : public ::testing::Test {
 protected:
  void
  SetUp() override {
    /*
     * Create a simple BBF topology for testing
     */
    topology_ = TopologyGenerator::createBbfSimple(
        numSpines_,
        numLeafs_,
        numPodsPerLeaf_,
        numPrefixesPerRouter_,
        numPrefixesPerPod_);

    /*
     * Collect neighbor names (spines)
     */
    for (int i = 0; i < numSpines_; ++i) {
      neighborNames_.push_back(fmt::format("spine-{}", i));
    }
  }

  int numSpines_{4};
  int numLeafs_{8};
  int numPodsPerLeaf_{2};
  int numPrefixesPerRouter_{10};
  int numPrefixesPerPod_{5};

  Topology topology_;
  std::vector<std::string> neighborNames_;
};

/*
 * Test KvStoreDataBuilder basic functionality
 */
TEST_F(FakeKvStoreTest, DataBuilderBuildsForNeighbor) {
  auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);

  /*
   * Should have at least the adj key for spine-0
   */
  EXPECT_GT(kvData.size(), 0);

  /*
   * Check that spine-0's adj key exists with correct originator
   */
  auto adjKey = fmt::format("adj:spine-0");
  auto it = kvData.find(adjKey);
  ASSERT_NE(it, kvData.end()) << "Missing adj key for spine-0";
  EXPECT_EQ(*it->second.originatorId(), "spine-0");
}

TEST_F(FakeKvStoreTest, DataBuilderBuildsForAllNeighbors) {
  auto allKvData =
      KvStoreDataBuilder::buildForAllNeighbors(neighborNames_, topology_);

  EXPECT_EQ(allKvData.size(), neighborNames_.size());

  /*
   * Each neighbor should have unique adj key with their own originatorId
   */
  for (const auto& name : neighborNames_) {
    auto it = allKvData.find(name);
    ASSERT_NE(it, allKvData.end()) << "Missing KV data for " << name;

    auto adjKey = fmt::format("adj:{}", name);
    auto adjIt = it->second.find(adjKey);
    ASSERT_NE(adjIt, it->second.end()) << "Missing adj key for " << name;
    EXPECT_EQ(*adjIt->second.originatorId(), name);
  }
}

/*
 * Test FakeKvStoreHandler sync operations
 */
TEST_F(FakeKvStoreTest, HandlerReturnsAllKeysOnEmptyHashes) {
  auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
  size_t originalKeyCount = kvData.size();

  auto handler =
      std::make_shared<FakeKvStoreHandler>("spine-0", std::move(kvData));
  auto client = apache::thrift::makeTestClient(handler);

  /*
   * Request with no key hashes should return all keys
   */
  thrift::KeyDumpParams filter;
  auto result =
      client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

  /*
   * Should return all keys since we have no hashes
   */
  EXPECT_EQ(result.keyVals()->size(), originalKeyCount);
}

TEST_F(FakeKvStoreTest, HandlerReturnsDiffOnPartialHashes) {
  auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
  size_t originalKeyCount = kvData.size();

  auto handler =
      std::make_shared<FakeKvStoreHandler>("spine-0", std::move(kvData));
  auto client = apache::thrift::makeTestClient(handler);

  /*
   * Build hashes for half the keys (simulate DUT having some keys)
   */
  thrift::KeyDumpParams filter;
  filter.keyValHashes() = thrift::KeyVals();

  auto fullKvData = KvStoreDataBuilder::buildForNeighbor(topology_);
  size_t hashCount = 0;
  for (const auto& [key, value] : fullKvData) {
    if (hashCount >= originalKeyCount / 2) {
      break;
    }
    (*filter.keyValHashes())[key] = value;
    ++hashCount;
  }

  auto result =
      client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

  /*
   * Should return keys that weren't in our hashes
   */
  EXPECT_GT(result.keyVals()->size(), 0);
  EXPECT_LE(result.keyVals()->size(), originalKeyCount - hashCount);
}

TEST_F(FakeKvStoreTest, HandlerUpdatesKey) {
  auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
  auto handler =
      std::make_shared<FakeKvStoreHandler>("spine-0", std::move(kvData));
  auto client = apache::thrift::makeTestClient(handler);

  /*
   * Create a new key-value
   */
  thrift::Value newValue;
  newValue.version() = 99;
  newValue.originatorId() = "test-originator";
  newValue.value() = "test-value";

  handler->updateKey("test-key", newValue);

  /*
   * Verify the key is now in the store
   */
  thrift::KeyDumpParams filter;
  auto result =
      client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

  auto it = result.keyVals()->find("test-key");
  ASSERT_NE(it, result.keyVals()->end());
  EXPECT_EQ(*it->second.version(), 99);
  EXPECT_EQ(*it->second.originatorId(), "test-originator");
}

TEST_F(FakeKvStoreTest, HandlerRemovesKey) {
  auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);

  /*
   * Get one key to remove (use begin iterator)
   */
  ASSERT_GT(kvData.size(), 0);
  std::string keyToRemove = kvData.begin()->first;

  auto handler =
      std::make_shared<FakeKvStoreHandler>("spine-0", std::move(kvData));
  auto client = apache::thrift::makeTestClient(handler);
  handler->removeKey(keyToRemove);

  /*
   * Verify the key is gone
   */
  thrift::KeyDumpParams filter;
  auto result =
      client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

  EXPECT_EQ(result.keyVals()->find(keyToRemove), result.keyVals()->end());
}

/*
 * Test FakeKvStoreManager basic operations
 */
TEST_F(FakeKvStoreTest, ManagerAssignsUniquePorts) {
  auto base = 0;
  FakeKvStoreManager manager(base, 2);

  auto kvData1 = KvStoreDataBuilder::buildForNeighbor(topology_);
  auto kvData2 = KvStoreDataBuilder::buildForNeighbor(topology_);

  uint16_t port1 = manager.addNeighbor("spine-0", std::move(kvData1));
  uint16_t port2 = manager.addNeighbor("spine-1", std::move(kvData2));

  EXPECT_EQ(port1, base);
  EXPECT_EQ(port2, base + 1);
  EXPECT_NE(port1, port2);
}

TEST_F(FakeKvStoreTest, ManagerGetPortThrowsForUnknown) {
  FakeKvStoreManager manager(0, 2);

  EXPECT_THROW(manager.getPort("nonexistent"), std::runtime_error);
}

TEST_F(FakeKvStoreTest, ManagerStartsAndStops) {
  FakeKvStoreManager manager(0, 2);

  auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
  manager.addNeighbor("spine-0", std::move(kvData));

  EXPECT_FALSE(manager.isRunning());

  manager.start();
  EXPECT_TRUE(manager.isRunning());

  /*
   * Give server time to bind
   */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  manager.stop();
  EXPECT_FALSE(manager.isRunning());
}

TEST_F(FakeKvStoreTest, ManagerPropagatesKeyToAllNeighbors) {
  FakeKvStoreManager manager(0, 2);

  for (const auto& name : neighborNames_) {
    auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
    manager.addNeighbor(name, std::move(kvData));
  }

  /*
   * Propagate a new key
   */
  thrift::Value newValue;
  newValue.version() = 42;
  newValue.originatorId() = "propagated";
  newValue.value() = "propagated-value";

  manager.propagateKeyUpdate("propagated-key", newValue);

  /*
   * Verify all handlers have the key
   */
  for (const auto& name : neighborNames_) {
    auto handler = manager.getHandler(name);
    ASSERT_NE(handler, nullptr);

    auto client = apache::thrift::makeTestClient(handler);
    thrift::KeyDumpParams filter;
    auto result =
        client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

    auto it = result.keyVals()->find("propagated-key");
    ASSERT_NE(it, result.keyVals()->end())
        << "Neighbor " << name << " missing propagated key";
    EXPECT_EQ(*it->second.version(), 42);
  }
}

/*
 * Test topology change simulation
 */
TEST_F(FakeKvStoreTest, SimulateLinkFlap) {
  FakeKvStoreManager manager(0, 2);

  for (const auto& name : neighborNames_) {
    auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
    manager.addNeighbor(name, std::move(kvData));
  }

  /*
   * Simulate link flap on spine-0 (removes adjacency to dut)
   */
  manager.simulateLinkFlap("spine-0", "dut", topology_, false);

  /*
   * All handlers should have updated adj:spine-0 key
   */
  for (const auto& name : neighborNames_) {
    auto handler = manager.getHandler(name);
    ASSERT_NE(handler, nullptr);

    auto client = apache::thrift::makeTestClient(handler);
    thrift::KeyDumpParams filter;
    auto result =
        client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

    auto it = result.keyVals()->find("adj:spine-0");
    ASSERT_NE(it, result.keyVals()->end());

    /*
     * Version should have been bumped
     */
    EXPECT_GE(*it->second.version(), 2);
  }
}

TEST_F(FakeKvStoreTest, SimulateNodeRemoval) {
  FakeKvStoreManager manager(0, 2);

  for (const auto& name : neighborNames_) {
    auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
    manager.addNeighbor(name, std::move(kvData));
  }

  /*
   * Simulate removal of spine-1
   */
  manager.simulateNodeRemoval("spine-1");

  /*
   * All handlers should have updated (overloaded, empty) adj:spine-1 key
   */
  for (const auto& name : neighborNames_) {
    auto handler = manager.getHandler(name);
    ASSERT_NE(handler, nullptr);

    auto client = apache::thrift::makeTestClient(handler);
    thrift::KeyDumpParams filter;
    auto result =
        client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

    auto it = result.keyVals()->find("adj:spine-1");
    ASSERT_NE(it, result.keyVals()->end());
    EXPECT_GE(*it->second.version(), 2);
  }
}

/*
 * Test Thrift client connectivity (integration test)
 */
TEST_F(FakeKvStoreTest, ThriftClientCanConnect) {
  FakeKvStoreManager manager(0, 2);

  auto kvData = KvStoreDataBuilder::buildForNeighbor(topology_);
  manager.addNeighbor("spine-0", std::move(kvData));

  /*
   * Use makeTestClient to test handler directly (no network bind needed)
   */
  auto handler = manager.getHandler("spine-0");
  ASSERT_NE(handler, nullptr);

  auto client = apache::thrift::makeTestClient(handler);
  thrift::KeyDumpParams filter;
  auto result =
      client->semifuture_getKvStoreKeyValsFilteredArea(filter, "0").get();

  EXPECT_GT(result.keyVals()->size(), 0);
}

} // namespace openr
