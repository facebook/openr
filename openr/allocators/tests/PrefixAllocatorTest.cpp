/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockSystemServiceHandler.h"

#include <atomic>
#include <mutex>

#include <fbzmq/zmq/Common.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/prefix-manager/PrefixManager.h>

using namespace std;
using namespace folly;
using namespace openr;

DEFINE_int32(seed_prefix_len, 125, "length of seed prefix");

// interval for periodic syncs
const std::chrono::milliseconds kSyncInterval(10);

// key marker for allocating prefix
const AllocPrefixMarker kAllocPrefixMarker{"allocprefix:"};

// length of allocated prefix
const int kAllocPrefixLen = 128;

class PrefixAllocatorFixture : public ::testing::TestWithParam<bool> {
 public:
  void
  SetUp() override {
    // threadID constant
    const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

    // start openrEvb thread for KvStoreClient usage
    evbThread_ = std::thread([&]() { evb_.run(); });

    auto tConfig = getBasicOpenrConfig(myNodeName_);
    config_ = std::make_shared<Config>(tConfig);

    // Start KvStore and attach a client to it
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(
        zmqContext_,
        config_,
        std::unordered_map<std::string, openr::thrift::PeerSpec>{});
    kvStoreWrapper_->run();
    evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
      kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
          &evb_, myNodeName_, kvStoreWrapper_->getKvStore());
    });
    LOG(INFO) << "The test store is running";

    // Start persistent config store
    tempFileName_ = folly::sformat("/tmp/openr.{}", tid);
    configStore_ = std::make_unique<PersistentStore>(
        "1", tempFileName_, zmqContext_, true /* dryrun */);
    threads_.emplace_back([&]() noexcept { configStore_->run(); });
    configStore_->waitUntilRunning();

    // Erase previous configs (if any)
    configStore_->erase("prefix-allocator-config").get();
    configStore_->erase("prefix-manager-config").get();

    mockServiceHandler_ = std::make_shared<MockSystemServiceHandler>();
    server_ = std::make_shared<apache::thrift::ThriftServer>();
    server_->setNumIOWorkerThreads(1);
    server_->setNumAcceptThreads(1);
    server_->setPort(0);
    server_->setInterface(mockServiceHandler_);

    systemThriftThread_.start(server_);
    port_ = systemThriftThread_.getAddress()->getPort();

    // Create prefixMgr
    createPrefixManager();

    // Create PrefixAllocator
    createPrefixAllocator();
  }

  void
  createPrefixAllocator() {
    prefixAllocator_ = make_unique<PrefixAllocator>(
        myNodeName_,
        kvStoreWrapper_->getKvStore(),
        prefixUpdatesQueue_,
        MonitorSubmitUrl{"inproc://monitor_submit"},
        kAllocPrefixMarker,
        GetParam() ? PrefixAllocatorMode(PrefixAllocatorModeStatic())
                   : PrefixAllocatorMode(PrefixAllocatorModeSeeded()),
        false /* set loopback addr */,
        false /* override global address */,
        "" /* loopback interface name */,
        false /* prefix fwd type MPLS */,
        false /* prefix fwd algo KSP2_ED_ECMP */,
        kSyncInterval,
        configStore_.get(),
        zmqContext_,
        port_);
    threads_.emplace_back([&]() noexcept { prefixAllocator_->run(); });
    prefixAllocator_->waitUntilRunning();
  }

  void
  createPrefixManager() {
    prefixManager_ = std::make_unique<PrefixManager>(
        prefixUpdatesQueue_.getReader(),
        config_,
        configStore_.get(),
        kvStoreWrapper_->getKvStore(),
        false /* prefix-manager perf measurement */,
        std::chrono::seconds(0),
        false /* perPrefixKeys */);
    threads_.emplace_back([&]() noexcept {
      LOG(INFO) << "PrefixManager started. TID: " << std::this_thread::get_id();
      prefixManager_->run();
    });
    prefixManager_->waitUntilRunning();
  }

  void
  TearDown() override {
    prefixUpdatesQueue_.close();
    kvStoreWrapper_->closeQueue();

    kvStoreClient_.reset();

    // Stop various modules
    prefixAllocator_->stop();
    prefixAllocator_->waitUntilStopped();
    prefixAllocator_.reset();

    prefixManager_->stop();
    prefixManager_->waitUntilStopped();
    prefixManager_.reset();

    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();

    configStore_->stop();
    configStore_->waitUntilStopped();
    configStore_.reset();

    evb_.stop();
    evb_.waitUntilStopped();
    evbThread_.join();

    // Join for all threads to finish
    for (auto& thread : threads_) {
      thread.join();
    }

    // delete tempfile name
    ::unlink(tempFileName_.c_str());
    systemThriftThread_.stop();
  }

 protected:
  // ZMQ Context for IO processing
  fbzmq::Context zmqContext_;

  OpenrEventBase evb_;
  std::thread evbThread_;

  const std::string myNodeName_{"test-node"};
  std::string tempFileName_;

  std::shared_ptr<Config> config_;
  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;
  std::unique_ptr<PersistentStore> configStore_;
  std::unique_ptr<PrefixManager> prefixManager_;
  std::unique_ptr<PrefixAllocator> prefixAllocator_;

  std::vector<std::thread> threads_;

  // Queue for publishing prefix-updates to PrefixManager
  messaging::ReplicateQueue<thrift::PrefixUpdateRequest> prefixUpdatesQueue_;

  // create serializer object for parsing kvstore key/values
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<MockSystemServiceHandler> mockServiceHandler_;
  int32_t port_{0};
  std::shared_ptr<apache::thrift::ThriftServer> server_;
  apache::thrift::util::ScopedServerThread systemThriftThread_;
};

class PrefixAllocTest : public ::testing::TestWithParam<bool> {
 public:
  void
  SetUp() override {
    mockServiceHandler_ = std::make_shared<MockSystemServiceHandler>();
    server_ = std::make_shared<apache::thrift::ThriftServer>();
    server_->setNumIOWorkerThreads(1);
    server_->setNumAcceptThreads(1);
    server_->setPort(0);
    server_->setInterface(mockServiceHandler_);

    systemThriftThread_.start(server_);
    port_ = systemThriftThread_.getAddress()->getPort();
  }

  void
  TearDown() override {
    systemThriftThread_.stop();
  }

 protected:
  std::shared_ptr<MockSystemServiceHandler> mockServiceHandler_;
  int32_t port_{0};
  std::shared_ptr<apache::thrift::ThriftServer> server_;
  apache::thrift::util::ScopedServerThread systemThriftThread_;
};

INSTANTIATE_TEST_CASE_P(
    EmptySeedPrefixInstance, PrefixAllocTest, ::testing::Bool());

// ensure no duplicate prefix assigned when there are enough available
// and report insufficient when there are not
TEST_P(PrefixAllocTest, UniquePrefixes) {
  // seed prefix provided in constructor or not
  auto emptySeedPrefix = GetParam();

  // threadID constant
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

  // create serializer object for parsing kvstore key/values
  apache::thrift::CompactSerializer serializer;

  // Create seed prefix
  const auto seedPrefix = folly::IPAddress::createNetwork(
      folly::sformat("fc00:cafe:babe::/{}", FLAGS_seed_prefix_len));
  const auto newSeedPrefix = folly::IPAddress::createNetwork(
      folly::sformat("fc00:cafe:b00c::/{}", FLAGS_seed_prefix_len));

  std::optional<PrefixAllocatorParams> maybeAllocParams;
  if (!emptySeedPrefix) {
    maybeAllocParams = std::make_pair(seedPrefix, kAllocPrefixLen);
  }

  // allocate all subprefixes
  auto numAllocators = 0x1U << (kAllocPrefixLen - FLAGS_seed_prefix_len);

  // ZMQ Context for IO processing
  fbzmq::Context zmqContext;

  vector<uint32_t> lastPrefixes;

  vector<string> tempFileNames;
  SCOPE_EXIT {
    // delete the original temp file
    for (const auto& tempFileName : tempFileNames) {
      ::unlink(tempFileName.c_str());
    }
  };

  // restart allocators in round 1 and see if they retain their previous
  // prefixes in round 0
  for (auto round = 0; round < 2; ++round) {
    // Create another OpenrEventBase instance for looping clients
    // Put in outer scope of kvstore client to ensure it's still alive when the
    // client is destroyed
    OpenrEventBase evl;
    std::atomic<bool> shouldWait{true};
    std::atomic<bool> usingNewSeedPrefix{false};

    vector<std::shared_ptr<Config>> configs;
    vector<std::unique_ptr<PersistentStore>> configStores;
    vector<std::unique_ptr<PrefixManager>> prefixManagers;
    vector<messaging::ReplicateQueue<thrift::PrefixUpdateRequest>> prefixQueues{
        numAllocators};
    vector<std::unique_ptr<PrefixAllocator>> allocators;
    vector<thread> threads;

    // needed while allocators manipulate test state
    folly::Synchronized<std::unordered_map<
        std::string /* nodeId */,
        folly::CIDRNetwork /* seed prefix */>>
        nodeToPrefix;
    auto prefixDbCb = [&](
        std::string const& /* key */,
        std::optional<thrift::Value> value) mutable noexcept {
      // Parse PrefixDb
      ASSERT_TRUE(value.has_value());
      ASSERT_TRUE(value.value().value_ref().has_value());
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value.value().value_ref().value(), serializer);
      auto& prefixes = prefixDb.prefixEntries;

      // Verify some expectations
      EXPECT_GE(1, prefixes.size());
      if (prefixes.size()) {
        EXPECT_EQ(thrift::PrefixType::PREFIX_ALLOCATOR, prefixes[0].type);
        auto prefix = toIPNetwork(prefixes[0].prefix);
        EXPECT_EQ(kAllocPrefixLen, prefix.second);
        if (usingNewSeedPrefix) {
          EXPECT_TRUE(
              prefix.first.inSubnet(newSeedPrefix.first, newSeedPrefix.second));
        } else {
          EXPECT_TRUE(
              prefix.first.inSubnet(seedPrefix.first, seedPrefix.second));
        }

        // Add to our entry and check for termination condition
        SYNCHRONIZED(nodeToPrefix) {
          nodeToPrefix[prefixDb.thisNodeName] = prefix;

          // Check for termination condition
          if (nodeToPrefix.size() == numAllocators) {
            std::unordered_set<folly::CIDRNetwork> allPrefixes;
            for (auto const& kv : nodeToPrefix) {
              allPrefixes.emplace(kv.second);
            }
            if (allPrefixes.size() == numAllocators) {
              shouldWait.store(false, std::memory_order_relaxed);
            }
          }
        } // SYNCHRONIZED
      } else {
        nodeToPrefix->erase(prefixDb.thisNodeName);
      }
    };

    //
    // 1) spin up a kvstore and create KvStoreClientInternal
    //

    const auto nodeId = folly::sformat("test_store{}", round);

    auto tConfig = getBasicOpenrConfig(nodeId);
    auto config = std::make_shared<Config>(tConfig);
    auto store = std::make_shared<KvStoreWrapper>(
        zmqContext,
        config,
        std::unordered_map<std::string, openr::thrift::PeerSpec>{});
    store->run();
    LOG(INFO) << "The test store is running";

    // Attach a kvstore client in main event loop
    auto kvStoreClient = std::make_unique<KvStoreClientInternal>(
        &evl, nodeId, store->getKvStore());

    // Set seed prefix in KvStore
    if (emptySeedPrefix) {
      // inject seed prefix
      auto prefixAllocParam = folly::sformat(
          "{},{}",
          folly::IPAddress::networkToString(seedPrefix),
          kAllocPrefixLen);
      auto res = kvStoreClient->setKey(
          Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
      EXPECT_TRUE(res.has_value());
    }

    //
    // 2) start threads for allocators
    //

    // start event loop
    threads.emplace_back([&evl]() noexcept { evl.run(); });
    evl.waitUntilRunning();

    for (uint32_t i = 0; i < numAllocators; ++i) {
      const auto myNodeName = sformat("node-{}", i);

      // subscribe to prefixDb updates from KvStore for node
      kvStoreClient->subscribeKey(
          sformat("prefix:{}", myNodeName), prefixDbCb, false);

      // get a unique temp file name
      auto tempFileName = folly::sformat("/tmp/openr.{}.{}", tid, i);
      tempFileNames.emplace_back(tempFileName);

      // spin up config store server for this allocator
      auto configStore = std::make_unique<PersistentStore>(
          folly::sformat("node{}", i), tempFileName, zmqContext);
      threads.emplace_back([&configStore]() noexcept { configStore->run(); });
      configStore->waitUntilRunning();

      // Temporary config store for PrefixManager so that they are not being
      // used
      tempFileName = folly::sformat("/tmp/openr.{}.{}.{}", tid, round, i);
      tempFileNames.emplace_back(tempFileName);
      auto tempConfigStore = std::make_unique<PersistentStore>(
          folly::sformat("temp-node{}", i), tempFileName, zmqContext);
      threads.emplace_back([&tempConfigStore]() noexcept {
        tempConfigStore->run();
      });
      tempConfigStore->waitUntilRunning();

      auto currConfig =
          std::make_shared<Config>(getBasicOpenrConfig(myNodeName));
      // spin up prefix manager
      auto prefixManager = std::make_unique<PrefixManager>(
          prefixQueues.at(i).getReader(),
          currConfig,
          tempConfigStore.get(),
          store->getKvStore(),
          false /* prefix-manager perf measurement */,
          std::chrono::seconds(0),
          false /* perPrefixKeys */);
      threads.emplace_back([&prefixManager]() noexcept {
        prefixManager->run();
      });
      prefixManager->waitUntilRunning();
      prefixManagers.emplace_back(std::move(prefixManager));
      configs.emplace_back(std::move(currConfig));

      auto allocator = make_unique<PrefixAllocator>(
          myNodeName,
          store->getKvStore(),
          prefixQueues.at(i),
          MonitorSubmitUrl{"inproc://monitor_submit"},
          kAllocPrefixMarker,
          maybeAllocParams.has_value()
              ? PrefixAllocatorMode(*maybeAllocParams)
              : PrefixAllocatorMode(PrefixAllocatorModeSeeded()),
          false /* set loopback addr */,
          false /* override global address */,
          "" /* loopback interface name */,
          false /* prefix fwd type MPLS */,
          false /* prefix fwd algo KSP2_ED_ECMP */,
          kSyncInterval,
          configStore.get(),
          zmqContext,
          port_);
      threads.emplace_back([&allocator]() noexcept { allocator->run(); });
      allocator->waitUntilRunning();

      configStores.emplace_back(std::move(configStore));
      configStores.emplace_back(std::move(tempConfigStore));
      allocators.emplace_back(std::move(allocator));
    }

    //
    // 3) Now the distributed prefix allocation logic would kick in
    //

    LOG(INFO) << "waiting for full allocation to complete";
    while (shouldWait.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }

    //
    // 4) Change network prefix if seeded via KvStore and wait for new
    // allocation
    //
    if (emptySeedPrefix) {
      // clear previous state
      shouldWait.store(true, std::memory_order_relaxed);
      usingNewSeedPrefix.store(true, std::memory_order_relaxed);
      nodeToPrefix->clear();

      // announce new seed prefix
      auto prefixAllocParam = folly::sformat(
          "{},{}",
          folly::IPAddress::networkToString(newSeedPrefix),
          kAllocPrefixLen);
      auto res = kvStoreClient->setKey(
          Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
      EXPECT_TRUE(res.has_value());

      // wait for prefix allocation to finish
      LOG(INFO) << "waiting for full allocation to complete with new "
                << "seed prefix";
      while (shouldWait.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
      }
    }

    //
    // Stop eventloop and wait for it.
    //
    store->closeQueue();
    evl.stop();
    evl.waitUntilStopped();

    LOG(INFO) << "Stop allocators";
    for (auto& allocator : allocators) {
      allocator->stop();
      allocator->waitUntilStopped();
    }
    LOG(INFO) << "Stop all prefix update queues";
    for (auto& queue : prefixQueues) {
      queue.close();
    }
    LOG(INFO) << "Stop all prefix managers";
    for (auto& prefixManager : prefixManagers) {
      prefixManager->stop();
      prefixManager->waitUntilStopped();
    }
    LOG(INFO) << "Stop config store servers";
    for (auto& server : configStores) {
      server->stop();
      server->waitUntilStopped();
    }
    LOG(INFO) << "Join all threads";
    for (auto& t : threads) {
      t.join();
    }

    LOG(INFO) << "Stopping my store";
    store->stop();
    LOG(INFO) << "My store stopped";

    // Verify at the end when everything is stopped
    for (uint32_t i = 0; i < numAllocators; ++i) {
      if (round == 0) {
        // save it to be compared against in round 1
        auto index = allocators[i]->getMyPrefixIndex();
        ASSERT_TRUE(index.has_value());
        lastPrefixes.emplace_back(*index);
      } else {
        auto index = allocators[i]->getMyPrefixIndex();
        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(lastPrefixes[i], *index);
      }

      const auto myNodeName = sformat("node-{}", i);
      const auto prefix = getNthPrefix(
          usingNewSeedPrefix ? newSeedPrefix : seedPrefix,
          kAllocPrefixLen,
          lastPrefixes[i]);
      ASSERT_EQ(1, nodeToPrefix->count(myNodeName));
      ASSERT_EQ(prefix, nodeToPrefix->at(myNodeName));
    }
  } // for
}

/**
 * This test aims at testing clearing previous allocation on failure to obtain
 * allocation parameters from KvStore and ConfigStore as well as update of
 * seed prefix.
 */
TEST_P(PrefixAllocatorFixture, UpdateAllocation) {
  // Return immediately if static allocation parameter is set to true
  if (GetParam()) {
    return;
  }

  folly::Synchronized<std::optional<folly::CIDRNetwork>> allocPrefix;
  std::atomic<bool> hasAllocPrefix{false};
  const uint8_t allocPrefixLen = 24;
  const std::string subscriptionKey = folly::sformat(
      "{}{}", openr::Constants::kPrefixDbMarker.toString(), myNodeName_);

  // Set callback
  auto cb = [&](const std::string&, std::optional<thrift::Value> value) {
    // Parse PrefixDb
    ASSERT_TRUE(value.has_value());
    ASSERT_TRUE(value.value().value_ref().has_value());
    auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
        value.value().value_ref().value(), serializer);
    auto& prefixes = prefixDb.prefixEntries;

    // Verify some expectations
    EXPECT_GE(1, prefixes.size());
    if (prefixes.empty()) {
      SYNCHRONIZED(allocPrefix) {
        allocPrefix = std::nullopt;
      }
      LOG(INFO) << "Lost allocated prefix!";
      hasAllocPrefix.store(false, std::memory_order_relaxed);
    } else {
      EXPECT_EQ(thrift::PrefixType::PREFIX_ALLOCATOR, prefixes[0].type);
      auto prefix = toIPNetwork(prefixes[0].prefix);
      EXPECT_EQ(allocPrefixLen, prefix.second);
      SYNCHRONIZED(allocPrefix) {
        allocPrefix = prefix;
      }
      LOG(INFO) << "Got new prefix allocation!";
      hasAllocPrefix.store(true, std::memory_order_relaxed);
    }
  };

  kvStoreClient_->subscribeKey(subscriptionKey, cb, false);

  //
  // 1) Set seed prefix in kvStore and verify that we get an elected prefix
  //
  // announce new seed prefix
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  const auto seedPrefix = folly::IPAddress::createNetwork("10.1.0.0/16");
  auto prefixAllocParam = folly::sformat(
      "{},{}", folly::IPAddress::networkToString(seedPrefix), allocPrefixLen);
  auto res = kvStoreClient_->setKey(
      Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
  EXPECT_TRUE(res.has_value());
  // busy loop until we have prefix
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.has_value());
    if (allocPrefix.has_value()) {
      EXPECT_EQ(allocPrefixLen, allocPrefix->second);
      EXPECT_TRUE(allocPrefix->first.inSubnet("10.1.0.0/16"));
    }
  }
  LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

  //
  // 2) Clear prefix-allocator, clear prefix in config store and config store
  // and restart prefix allcoator. Expect prefix to be withdrawn
  //
  // ATTN: Clean up kvStoreUpdatesQueue before shutting down prefixAllocator.
  //       resetQueue() will clean up ALL readers including kvStoreClient. Must
  //       recreate it to receive KvStore publication.
  prefixUpdatesQueue_.close();
  kvStoreWrapper_->closeQueue();
  kvStoreClient_.reset();
  prefixAllocator_->stop();
  prefixAllocator_->waitUntilStopped();
  prefixAllocator_.reset();
  prefixManager_->stop();
  prefixManager_->waitUntilStopped();
  prefixManager_.reset();

  // reopen queue and restart prefixAllocator/prefixManager
  prefixUpdatesQueue_.open();
  kvStoreWrapper_->openQueue();
  evb_.getEvb()->runInEventBaseThreadAndWait([&]() noexcept {
    kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
        &evb_, myNodeName_, kvStoreWrapper_->getKvStore());
    // Set callback
    kvStoreClient_->subscribeKey(subscriptionKey, cb, false);
    kvStoreClient_->setKey(
        Constants::kSeedPrefixAllocParamKey.toString(),
        "",
        0,
        std::chrono::milliseconds(10)); // erase-key
    kvStoreClient_->unsetKey(Constants::kSeedPrefixAllocParamKey.toString());
  });
  configStore_->erase("prefix-allocator-config").get();
  // wait long enough for key to expire
  // @lint-ignore HOWTOEVEN1
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  hasAllocPrefix.store(true, std::memory_order_relaxed);
  createPrefixManager();
  createPrefixAllocator();
  while (hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(allocPrefix->has_value());
  LOG(INFO) << "Step-2: Lost allocated prefix";

  //
  // 3) Set prefix and expect new prefix to be elected
  //
  hasAllocPrefix.store(false, std::memory_order_relaxed);

  auto res2 = kvStoreClient_->setKey(
      Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
  EXPECT_TRUE(res2.has_value());
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(allocPrefix->has_value());
  LOG(INFO) << "Step-3: Received allocated prefix from KvStore.";
}

/**
 * The following test allocates a prefix based on the allocParams, then
 * static allocation key is inserted with the prefix that's allocated.
 * When the static allocation key is received, prefix allocator should
 * detect a collion and reallocate a new prefix.
 */
TEST_P(PrefixAllocatorFixture, StaticPrefixUpdate) {
  // Return immediately if static allocation parameter is set to true
  if (GetParam()) {
    return;
  }

  folly::Synchronized<std::optional<folly::CIDRNetwork>> allocPrefix;
  folly::CIDRNetwork prevAllocPrefix;
  std::atomic<bool> hasAllocPrefix{false};
  const uint8_t allocPrefixLen = 64;
  const std::string subscriptionKey = folly::sformat(
      "{}{}", openr::Constants::kPrefixDbMarker.toString(), myNodeName_);

  // Set callback
  kvStoreClient_->subscribeKey(
      subscriptionKey,
      [&](const std::string& /* key */, std::optional<thrift::Value> value) {
        // Parse PrefixDb
        ASSERT_TRUE(value.has_value());
        ASSERT_TRUE(value.value().value_ref().has_value());
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            value.value().value_ref().value(), serializer);
        auto& prefixes = prefixDb.prefixEntries;

        // Verify some expectations
        EXPECT_GE(1, prefixes.size());
        if (prefixes.empty()) {
          SYNCHRONIZED(allocPrefix) {
            allocPrefix = std::nullopt;
          }
          LOG(INFO) << "Lost allocated prefix!";
          hasAllocPrefix.store(false, std::memory_order_relaxed);
        } else {
          EXPECT_EQ(thrift::PrefixType::PREFIX_ALLOCATOR, prefixes[0].type);
          auto prefix = toIPNetwork(prefixes[0].prefix);
          EXPECT_EQ(allocPrefixLen, prefix.second);
          SYNCHRONIZED(allocPrefix) {
            allocPrefix = prefix;
          }
          LOG(INFO) << "Got new prefix allocation!";
          hasAllocPrefix.store(true, std::memory_order_relaxed);
        } // if
      }, // callback
      false);

  //
  // 1) Set seed prefix in kvStore and verify that we get an elected prefix
  //
  // announce new seed prefix
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  std::string ip6{"face:b00c:d00d::/61"};
  const auto seedPrefix = folly::IPAddress::createNetwork(ip6);
  auto prefixAllocParam = folly::sformat(
      "{},{}", folly::IPAddress::networkToString(seedPrefix), allocPrefixLen);
  auto res = kvStoreClient_->setKey(
      Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
  EXPECT_TRUE(res.has_value());
  // busy loop until we have prefix
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.has_value());
    if (allocPrefix.has_value()) {
      EXPECT_EQ(allocPrefixLen, allocPrefix->second);
      EXPECT_TRUE(allocPrefix->first.inSubnet(ip6));
      prevAllocPrefix = allocPrefix.value();
    }
  }
  LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

  // now insert e2e-network-allocation with the allocated v6 address.
  // prefix allocator subscribes to this key and should detect address
  // collision, and restart the prefix allocator to assign a different address

  thrift::StaticAllocation staticAlloc;
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  staticAlloc.nodePrefixes[myNodeName_] =
      toIpPrefix(folly::IPAddress::networkToString(prevAllocPrefix));
  auto res0 = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer),
      1);
  EXPECT_TRUE(res0.has_value());

  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.has_value());
    if (allocPrefix.has_value()) {
      EXPECT_EQ(allocPrefixLen, allocPrefix->second);
      EXPECT_TRUE(allocPrefix->first.inSubnet(ip6));
      EXPECT_NE(prevAllocPrefix, allocPrefix.value());
    }
  }
  LOG(INFO) << "Step-2: Received allocated prefix from KvStore.";

  // statically all possible v6 addresses except one. Prefix allocator
  // must assign the one that's left out in the static list

  hasAllocPrefix.store(false, std::memory_order_relaxed);
  staticAlloc.nodePrefixes["dontcare0"] = toIpPrefix("face:b00c:d00d:0::/64");
  staticAlloc.nodePrefixes["dontcare1"] = toIpPrefix("face:b00c:d00d:1::/64");
  staticAlloc.nodePrefixes["dontcare2"] = toIpPrefix("face:b00c:d00d:2::/64");
  staticAlloc.nodePrefixes["dontcare3"] = toIpPrefix("face:b00c:d00d:3::/64");
  staticAlloc.nodePrefixes["dontcare4"] = toIpPrefix("face:b00c:d00d:4::/64");
  staticAlloc.nodePrefixes["dontcare6"] = toIpPrefix("face:b00c:d00d:6::/64");
  staticAlloc.nodePrefixes["dontcare7"] = toIpPrefix("face:b00c:d00d:7::/64");

  auto res5 = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer),
      2);
  EXPECT_TRUE(res5.has_value());

  // counter is added to break loop. In case allocated prefix is already
  // the expected one, there will be no prefix update and hasAllocPrefix
  // will remain 'false'
  auto ctr = 1000000;
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
    if (ctr-- < 0) {
      break;
    }
  }

  // check the prefix allocated is the only available prefix
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.has_value());
    if (allocPrefix.has_value()) {
      EXPECT_EQ(allocPrefixLen, allocPrefix->second);
      EXPECT_TRUE(allocPrefix->first.inSubnet(ip6));
      EXPECT_EQ(allocPrefix->first.str(), "face:b00c:d00d:5::");
    }
  }

  LOG(INFO) << "Step-3: Received allocated prefix from KvStore.";
}

/**
 * Tests static allocation mode of PrefixAllocator
 */
TEST_P(PrefixAllocatorFixture, StaticAllocation) {
  // Return immediately if static allocation parameter is set to false
  if (not GetParam()) {
    return;
  }

  thrift::StaticAllocation staticAlloc;
  folly::Synchronized<std::optional<folly::CIDRNetwork>> allocPrefix;
  std::atomic<bool> hasAllocPrefix{false};
  const std::string subscriptionKey =
      folly::sformat("{}{}", openr::Constants::kPrefixDbMarker, myNodeName_);

  // Set callback
  auto cb = [&](const std::string&, std::optional<thrift::Value> value) {
    // Parse PrefixDb
    ASSERT_TRUE(value.has_value());
    ASSERT_TRUE(value.value().value_ref().has_value());
    auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
        value.value().value_ref().value(), serializer);
    auto& prefixes = prefixDb.prefixEntries;

    // Verify some expectations
    EXPECT_GE(1, prefixes.size());
    if (prefixes.empty()) {
      SYNCHRONIZED(allocPrefix) {
        allocPrefix = std::nullopt;
      }
      LOG(INFO) << "Lost allocated prefix!";
      hasAllocPrefix.store(false, std::memory_order_relaxed);
    } else {
      EXPECT_EQ(thrift::PrefixType::PREFIX_ALLOCATOR, prefixes[0].type);
      auto prefix = toIPNetwork(prefixes[0].prefix);
      SYNCHRONIZED(allocPrefix) {
        allocPrefix = prefix;
      }
      LOG(INFO) << "Got new prefix allocation!";
      hasAllocPrefix.store(true, std::memory_order_relaxed);
    }
  };

  kvStoreClient_->subscribeKey(subscriptionKey, cb, false);

  //
  // 1) Set static allocation in KvStore
  //
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  staticAlloc.nodePrefixes[myNodeName_] = toIpPrefix("1.2.3.0/24");
  auto res = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer));
  EXPECT_TRUE(res.has_value());
  // busy loop until we have prefix
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.has_value());
    if (allocPrefix.has_value()) {
      EXPECT_EQ(allocPrefix, folly::IPAddress::createNetwork("1.2.3.0/24"));
    }
  }
  LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

  //
  // 2) Stop prefix-allocator, clear prefix in config store and config store
  //    and restart prefix allcoator. Expect prefix to be withdrawn
  //
  // ATTN: Clean up kvStoreUpdatesQueue before shutting down prefixAllocator.
  //       resetQueue() will clean up ALL readers including kvStoreClient. Must
  //       recreate it to receive KvStore publication.
  prefixUpdatesQueue_.close();
  kvStoreWrapper_->closeQueue();
  kvStoreClient_.reset();
  prefixAllocator_->stop();
  prefixAllocator_->waitUntilStopped();
  prefixAllocator_.reset();
  prefixManager_->stop();
  prefixManager_->waitUntilStopped();
  prefixManager_.reset();

  // reopen queue and restart prefixAllocator/prefixManager
  prefixUpdatesQueue_.open();
  kvStoreWrapper_->openQueue();
  evb_.getEvb()->runInEventBaseThreadAndWait([&]() noexcept {
    kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
        &evb_, myNodeName_, kvStoreWrapper_->getKvStore());
    // Set callback
    kvStoreClient_->subscribeKey(subscriptionKey, cb, false);
    kvStoreClient_->setKey(
        Constants::kStaticPrefixAllocParamKey.toString(),
        "",
        0,
        std::chrono::milliseconds(10)); // erase-key
    kvStoreClient_->unsetKey(Constants::kStaticPrefixAllocParamKey.toString());
  });
  configStore_->erase("prefix-allocator-config").get();
  // wait long enough for key to expire
  // @lint-ignore HOWTOEVEN1
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  hasAllocPrefix.store(true, std::memory_order_relaxed);
  createPrefixManager();
  createPrefixAllocator();
  while (hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(allocPrefix->has_value());
  LOG(INFO) << "Step-2: Lost allocated prefix";

  //
  // 3) Set prefix and expect new prefix to be advertised
  //
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  staticAlloc.nodePrefixes[myNodeName_] = toIpPrefix("3.2.1.0/24");
  auto res2 = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer));
  EXPECT_TRUE(res2.has_value());
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.has_value());
    if (allocPrefix.has_value()) {
      EXPECT_EQ(allocPrefix, folly::IPAddress::createNetwork("3.2.1.0/24"));
    }
  }
  LOG(INFO) << "Step-3: Received allocated prefix from KvStore.";

  //
  // 4) Change prefix in static config and expect the announcement
  //
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  staticAlloc.nodePrefixes[myNodeName_] = toIpPrefix("5.6.7.0/24");
  auto res3 = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer));
  EXPECT_TRUE(res3.has_value());
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.has_value());
    if (allocPrefix.has_value()) {
      EXPECT_EQ(allocPrefix, folly::IPAddress::createNetwork("5.6.7.0/24"));
    }
  }
  LOG(INFO) << "Step-4: Received updated allocated prefix from KvStore.";

  //
  // 5) Remove prefix in static config and expect the withdrawal
  //
  hasAllocPrefix.store(true, std::memory_order_relaxed);
  staticAlloc.nodePrefixes.erase(myNodeName_);
  auto res4 = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer));
  EXPECT_TRUE(res4.has_value());
  while (hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(allocPrefix->has_value());
  LOG(INFO) << "Step-5: Received withdraw for allocated prefix from KvStore.";
}

INSTANTIATE_TEST_CASE_P(FixtureTest, PrefixAllocatorFixture, ::testing::Bool());

TEST(PrefixAllocator, getPrefixCount) {
  {
    auto params =
        std::make_pair(folly::IPAddress::createNetwork("face::/56"), 64);
    EXPECT_EQ(256, PrefixAllocator::getPrefixCount(params));
  }
  {
    auto params =
        std::make_pair(folly::IPAddress::createNetwork("face::/64"), 64);
    EXPECT_EQ(1, PrefixAllocator::getPrefixCount(params));
  }
  {
    auto params =
        std::make_pair(folly::IPAddress::createNetwork("face::/16"), 64);
    EXPECT_EQ(1 << 31, PrefixAllocator::getPrefixCount(params));
  }
  {
    auto params =
        std::make_pair(folly::IPAddress::createNetwork("1.2.0.0/16"), 24);
    EXPECT_EQ(256, PrefixAllocator::getPrefixCount(params));
  }
}

TEST(PrefixAllocator, parseParamsStr) {
  // Missing subnet specification in seed-prefix
  {
    auto maybeParams = PrefixAllocator::parseParamsStr("face::,64");
    EXPECT_TRUE(maybeParams.hasError());
  }

  // Incorrect seed prefix
  {
    auto maybeParams = PrefixAllocator::parseParamsStr("face::b00c::/56,64");
    EXPECT_TRUE(maybeParams.hasError());
  }

  // Seed prefix same or greather than alloc prefix length (error case).
  {
    auto maybeParams = PrefixAllocator::parseParamsStr("face:b00c::/64,64");
    EXPECT_TRUE(maybeParams.hasError());
    auto maybeParams2 = PrefixAllocator::parseParamsStr("face:b00c::/74,64");
    EXPECT_TRUE(maybeParams2.hasError());
  }

  // Correct case - v6
  {
    auto maybeParams = PrefixAllocator::parseParamsStr("face::/56,64");
    EXPECT_FALSE(maybeParams.hasError());
    if (maybeParams.hasValue()) {
      EXPECT_EQ(
          folly::IPAddress::createNetwork("face::/56"), maybeParams->first);
      EXPECT_EQ(64, maybeParams->second);
    }
  }

  // Correct case - v4
  {
    // Note: last byte will be masked off
    auto maybeParams = PrefixAllocator::parseParamsStr("1.2.0.1/16,24");
    EXPECT_FALSE(maybeParams.hasError());
    if (maybeParams.hasValue()) {
      EXPECT_EQ(
          folly::IPAddress::createNetwork("1.2.0.0/16"), maybeParams->first);
      EXPECT_EQ(24, maybeParams->second);
    }
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
