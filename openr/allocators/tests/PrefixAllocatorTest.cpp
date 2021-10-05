/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <atomic>

#include <fbzmq/zmq/Zmq.h>
#include <folly/init/Init.h>
#include <folly/synchronization/Baton.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/tests/mocks/MockNetlinkProtocolSocket.h>
#include <openr/tests/utils/Utils.h>

using namespace openr;

// prefix length
const uint64_t kSeedPrefixLen(125);

// interval for periodic syncs
const std::chrono::milliseconds kSyncInterval(10);

// length of allocated prefix
const int kAllocPrefixLen = 128;

class PrefixAllocatorFixture : public ::testing::Test {
 public:
  void
  SetUp() override {}

  // Override SetUp() call with parameter passed in
  virtual void
  SetUp(thrift::PrefixAllocationMode mode) {
    // threadID constant
    const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

    // start openrEvb thread for KvStoreClient usage
    evbThread_ = std::thread([&]() { evb_.run(); });

    auto tConfig = getBasicOpenrConfig(myNodeName_);
    tConfig.enable_prefix_allocation_ref() = true;

    thrift::PrefixAllocationConfig pfxAllocationConf;
    pfxAllocationConf.loopback_interface_ref() = "";
    pfxAllocationConf.prefix_allocation_mode_ref() = mode;

    tConfig.prefix_allocation_config_ref() = pfxAllocationConf;
    tConfig.persistent_config_store_path_ref() =
        folly::sformat("/tmp/openr.{}", tid);
    config_ = std::make_shared<Config>(tConfig);

    // Start KvStore and attach a client to it
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(
        zmqContext_,
        config_,
        myPeerUpdatesQueue_.getReader(),
        kvRequestQueue_.getReader());
    kvStoreWrapper_->run();
    evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
      kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
          &evb_, myNodeName_, kvStoreWrapper_->getKvStore());
    });

    // Start persistent config store
    configStore_ =
        std::make_unique<PersistentStore>(config_, true /* dryrun */);
    configStoreThread_ = std::make_unique<std::thread>(
        [this]() noexcept { configStore_->run(); });
    configStore_->waitUntilRunning();

    // Erase previous configs (if any)
    configStore_->erase("prefix-allocator-config").get();
    configStore_->erase("prefix-manager-config").get();

    // create fakeNetlinkProtocolSocket
    folly::EventBase evb;
    nlSock_ = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&evb);

    // Create prefixMgr
    createPrefixManager();

    // Create PrefixAllocator
    createPrefixAllocator();
  }

  void
  createPrefixAllocator() {
    prefixAllocator_ = std::make_unique<PrefixAllocator>(
        kTestingAreaName,
        config_,
        nlSock_.get(),
        kvStoreWrapper_->getKvStore(),
        configStore_.get(),
        prefixUpdatesQueue_,
        logSampleQueue_,
        kvRequestQueue_,
        kSyncInterval);
    prefixAllocatorThread_ = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "PrefixAllocator started. TID: "
                << std::this_thread::get_id();
      prefixAllocator_->run();
    });
    prefixAllocator_->waitUntilRunning();
  }

  void
  createPrefixManager() {
    prefixManager_ = std::make_unique<PrefixManager>(
        staticRouteUpdatesQueue_,
        kvRequestQueue_,
        prefixMgrRouteUpdatesQueue_,
        kvStoreWrapper_->getReader(),
        prefixUpdatesQueue_.getReader(),
        fibRouteUpdatesQueue_.getReader(),
        config_,
        kvStoreWrapper_->getKvStore());
    prefixManagerThread_ = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "PrefixManager started. TID: " << std::this_thread::get_id();
      prefixManager_->run();
    });
    prefixManager_->waitUntilRunning();
  }

  void
  TearDown() override {
    staticRouteUpdatesQueue_.close();
    prefixUpdatesQueue_.close();
    prefixMgrRouteUpdatesQueue_.close();
    fibRouteUpdatesQueue_.close();
    kvRequestQueue_.close();
    logSampleQueue_.close();
    myPeerUpdatesQueue_.close();
    kvStoreWrapper_->closeQueue();

    kvStoreClient_->stop();
    kvStoreClient_.reset();

    // Stop various modules
    prefixAllocator_->stop();
    prefixAllocator_->waitUntilStopped();
    prefixAllocatorThread_->join();
    prefixAllocator_.reset();

    prefixManager_->stop();
    prefixManager_->waitUntilStopped();
    prefixManagerThread_->join();
    prefixManager_.reset();

    kvStoreWrapper_->stop();
    kvStoreWrapper_.reset();

    configStore_->stop();
    configStore_->waitUntilStopped();
    configStoreThread_->join();
    configStore_.reset();

    evb_.stop();
    evb_.waitUntilStopped();
    evbThread_.join();

    // destroy MockNetlinkProtocolSocket
    nlSock_.reset();

    // delete tempfile name
    ::unlink(tempFileName_.c_str());
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
  std::unique_ptr<std::thread> configStoreThread_;
  std::unique_ptr<std::thread> prefixManagerThread_;
  std::unique_ptr<std::thread> prefixAllocatorThread_;

  // Queue for publishing prefix-updates to PrefixManager
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue_;
  messaging::ReplicateQueue<PrefixEvent> prefixUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> prefixMgrRouteUpdatesQueue_;
  messaging::ReplicateQueue<DecisionRouteUpdate> fibRouteUpdatesQueue_;
  messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue_;
  messaging::ReplicateQueue<PeerEvent> myPeerUpdatesQueue_;

  // Queue for event logs
  messaging::ReplicateQueue<LogSample> logSampleQueue_;

  // create serializer object for parsing kvstore key/values
  apache::thrift::CompactSerializer serializer;

  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock_;
};

class PrefixAllocTest : public ::testing::TestWithParam<bool> {
 public:
  void
  SetUp() override {
    // create mockNetlinkProtocolSocket
    folly::EventBase evb;
    nlSock_ = std::make_unique<fbnl::MockNetlinkProtocolSocket>(&evb);
  }

  void
  TearDown() override {
    nlSock_.reset();

    // delete the original temp file
    for (const auto& tempFileName : tempFileNames_) {
      ::unlink(tempFileName.c_str());
    }
  }

 protected:
  std::unique_ptr<fbnl::MockNetlinkProtocolSocket> nlSock_;

  // prefixes saved in first round and compared in second round
  std::vector<uint32_t> lastPrefixes_;

  std::vector<std::string> tempFileNames_;

  // serializer object for parsing kvstore key/values
  apache::thrift::CompactSerializer serializer_;

  // ZMQ Context for IO processing
  fbzmq::Context zmqContext;
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

  // Create seed prefix
  const auto seedPrefix = folly::IPAddress::createNetwork(
      folly::sformat("fc00:cafe:babe::/{}", kSeedPrefixLen));
  const auto newSeedPrefix = folly::IPAddress::createNetwork(
      folly::sformat("fc00:cafe:b00c::/{}", kSeedPrefixLen));

  // allocate all subprefixes
  auto numAllocators = 0x1U << (kAllocPrefixLen - kSeedPrefixLen);

  LOG(INFO) << "Starting with: " << numAllocators
            << " independent prefixManagers and prefixAllocators...";

  // TODO: Refactor this long for-loop to make it readable.
  //
  // restart allocators in round 1 and see if they retain their previous
  // prefixes in round 0
  for (auto round = 0; round < 2; ++round) {
    // Create another OpenrEventBase instance for looping clients
    // Put in outer scope of kvstore client to ensure it's still alive when the
    // client is destroyed
    OpenrEventBase evb;
    std::thread evbThread;

    folly::Baton waitBaton;
    std::atomic<bool> usingNewSeedPrefix{false};

    std::vector<std::shared_ptr<Config>> configs;
    std::unique_ptr<KvStoreWrapper> kvStoreWrapper;
    std::unique_ptr<KvStoreClientInternal> kvStoreClient;
    std::vector<std::unique_ptr<PersistentStore>> configStores;
    std::vector<std::unique_ptr<PrefixManager>> prefixManagers;
    std::vector<std::unique_ptr<PrefixAllocator>> allocators;
    std::vector<std::thread> configStoreThreads;
    std::vector<std::thread> prefixManagerThreads;
    std::vector<std::thread> prefixAllocatorThreads;
    messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
    std::vector<messaging::ReplicateQueue<PrefixEvent>> prefixQueues{
        numAllocators};
    std::vector<messaging::ReplicateQueue<DecisionRouteUpdate>>
        fibRouteUpdatesQueues{numAllocators};
    messaging::ReplicateQueue<KeyValueRequest> kvRequestQueue;
    messaging::ReplicateQueue<LogSample> logSampleQueue;
    messaging::ReplicateQueue<DecisionRouteUpdate> prefixMgrRouteUpdatesQueue;

    // needed while allocators manipulate test state
    folly::Synchronized<std::unordered_map<std::string, folly::CIDRNetwork>>
        nodeToPrefix_;

    auto prefixDbCb = [&](std::string const&,
                          std::optional<thrift::Value> value) mutable noexcept {
      // Parse PrefixDb
      ASSERT_TRUE(value.has_value());
      ASSERT_TRUE(value.value().value_ref().has_value());
      auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
          value.value().value_ref().value(), serializer_);
      auto prefixes = *prefixDb.prefixEntries_ref();
      bool isPrefixDbDeleted = *prefixDb.deletePrefix_ref();

      // prefixDb marked by `deletedPrefix` to indicate prefix is stale.
      // ATTN: With per-prefix-key advertisement, `prefixEntries` can be
      //       non-empty when prefix is marked as deleted.
      EXPECT_GE(1, prefixes.size());
      if (isPrefixDbDeleted or prefixes.empty()) {
        // ignore prefix update if it is withdrawn or empty case
        return;
      }

      EXPECT_EQ(thrift::PrefixType::PREFIX_ALLOCATOR, *prefixes[0].type_ref());
      EXPECT_EQ(
          std::set<std::string>({"AUTO-ALLOCATED"}), prefixes.at(0).tags_ref());
      EXPECT_EQ(
          Constants::kDefaultPathPreference,
          prefixes.at(0).metrics_ref()->path_preference_ref());
      EXPECT_EQ(
          Constants::kDefaultSourcePreference,
          prefixes.at(0).metrics_ref()->source_preference_ref());

      auto prefix = toIPNetwork(*prefixes[0].prefix_ref());
      EXPECT_EQ(kAllocPrefixLen, prefix.second);
      EXPECT_TRUE(prefix.first.inSubnet(
          usingNewSeedPrefix ? newSeedPrefix.first : seedPrefix.first,
          kSeedPrefixLen));

      // Add to our entry and check for termination condition
      nodeToPrefix_.withWLock([&](auto& nodeToPrefix) {
        auto nodeName = *prefixDb.thisNodeName_ref();
        nodeToPrefix[nodeName] = prefix;

        LOG(INFO) << "Prefix: " << prefix.first
                  << " being allocated for node: " << nodeName;

        // Check for termination condition
        if (nodeToPrefix.size() == numAllocators) {
          std::unordered_set<folly::CIDRNetwork> allPrefixes;
          for (auto const& kv : nodeToPrefix) {
            allPrefixes.emplace(kv.second);
          }
          if (allPrefixes.size() == numAllocators) {
            LOG(INFO) << "All prefixes received!";

            // sycnronization primitive to mark prefixes received
            waitBaton.post();
          }
        } else {
          LOG(INFO)
              << "Waiting for prefixes to be allocated: " << nodeToPrefix.size()
              << " out of " << numAllocators << " tasks finished.";
        }
      });
    };

    // start event loop
    evbThread = std::thread([&evb]() noexcept { evb.run(); });
    evb.waitUntilRunning();

    //
    // 1) spin up a kvstore and create KvStoreClientInternal
    //
    const auto nodeId = folly::sformat("test_store{}", round);
    auto tConfig = getBasicOpenrConfig(nodeId);
    auto config = std::make_shared<Config>(tConfig);
    kvStoreWrapper = std::make_unique<KvStoreWrapper>(
        zmqContext, config, std::nullopt, kvRequestQueue.getReader());
    kvStoreWrapper->run();

    // Attach a kvstore client in main event loop
    evb.getEvb()->runInEventBaseThreadAndWait([&]() {
      kvStoreClient = std::make_unique<KvStoreClientInternal>(
          &evb, nodeId, kvStoreWrapper->getKvStore());

      // Set seed prefix in KvStore
      if (emptySeedPrefix) {
        // inject seed prefix
        auto prefixAllocParam = folly::sformat(
            "{},{}",
            folly::IPAddress::networkToString(seedPrefix),
            kAllocPrefixLen);
        auto res = kvStoreClient->setKey(
            kTestingAreaName,
            Constants::kSeedPrefixAllocParamKey.toString(),
            prefixAllocParam);
        EXPECT_TRUE(res.has_value());
      }
    });

    //
    // 2) start threads for allocators
    //
    for (uint32_t i = 0; i < numAllocators; ++i) {
      const auto myNodeName = folly::sformat("node-{}", i);

      // subscribe to prefixDb updates from KvStore for node
      evb.getEvb()->runInEventBaseThreadAndWait([&]() {
        KvStoreFilters kvStoreFilters(
            {Constants::kPrefixDbMarker.toString()}, {nodeId});

        kvStoreClient->subscribeKeyFilter(
            std::move(kvStoreFilters), prefixDbCb);
      });

      // get a unique temp file name
      auto tempFileName = folly::sformat("/tmp/openr.{}.{}", tid, i);
      auto tConfig1 = getBasicOpenrConfig();
      tConfig1.persistent_config_store_path_ref() = tempFileName;
      auto config1 = std::make_shared<Config>(tConfig1);
      tempFileNames_.emplace_back(tempFileName);

      // spin up config store server for this allocator
      auto configStore = std::make_unique<PersistentStore>(config1);
      configStoreThreads.emplace_back(
          [&configStore]() noexcept { configStore->run(); });
      configStore->waitUntilRunning();

      // Temporary config store for PrefixManager so that they are not being
      // used
      tempFileName = folly::sformat("/tmp/openr.{}.{}.{}", tid, round, i);
      auto tConfig2 = getBasicOpenrConfig();
      tConfig.persistent_config_store_path_ref() = tempFileName;
      auto config2 = std::make_shared<Config>(tConfig2);
      tempFileNames_.emplace_back(tempFileName);

      // spin up config store server for this allocator
      auto tempConfigStore = std::make_unique<PersistentStore>(config2);
      configStoreThreads.emplace_back(
          [&tempConfigStore]() noexcept { tempConfigStore->run(); });
      tempConfigStore->waitUntilRunning();

      auto currTConfig = getBasicOpenrConfig(myNodeName);
      currTConfig.enable_prefix_allocation_ref() = true;
      thrift::PrefixAllocationConfig pfxAllocationConf;
      *pfxAllocationConf.loopback_interface_ref() = "";
      pfxAllocationConf.prefix_allocation_mode_ref() =
          thrift::PrefixAllocationMode::DYNAMIC_LEAF_NODE;
      if (not emptySeedPrefix) {
        pfxAllocationConf.prefix_allocation_mode_ref() =
            thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE;
        pfxAllocationConf.seed_prefix_ref() =
            folly::sformat("fc00:cafe:babe::/{}", kSeedPrefixLen);
        pfxAllocationConf.allocate_prefix_len_ref() = kAllocPrefixLen;
      }
      currTConfig.prefix_allocation_config_ref() = pfxAllocationConf;
      auto currConfig = std::make_shared<Config>(currTConfig);

      // spin up prefix manager
      auto prefixManager = std::make_unique<PrefixManager>(
          staticRouteUpdatesQueue,
          kvRequestQueue,
          prefixMgrRouteUpdatesQueue,
          kvStoreWrapper->getReader(),
          prefixQueues.at(i).getReader(),
          fibRouteUpdatesQueues.at(i).getReader(),
          currConfig,
          kvStoreWrapper->getKvStore());
      prefixManagerThreads.emplace_back(
          [&prefixManager]() noexcept { prefixManager->run(); });
      prefixManager->waitUntilRunning();
      prefixManagers.emplace_back(std::move(prefixManager));

      auto allocator = std::make_unique<PrefixAllocator>(
          kTestingAreaName,
          currConfig,
          nlSock_.get(),
          kvStoreWrapper->getKvStore(),
          configStore.get(),
          prefixQueues.at(i),
          logSampleQueue,
          kvRequestQueue,
          kSyncInterval);
      prefixAllocatorThreads.emplace_back(
          [&allocator]() noexcept { allocator->run(); });
      allocator->waitUntilRunning();

      configStores.emplace_back(std::move(configStore));
      configStores.emplace_back(std::move(tempConfigStore));
      allocators.emplace_back(std::move(allocator));
      configs.emplace_back(std::move(currConfig));
    }

    //
    // 3) Now the distributed prefix allocation logic would kick in
    //
    LOG(INFO) << "Waiting for full allocation to complete...";
    waitBaton.wait();

    //
    // 4) Change network prefix if seeded via KvStore and wait for new
    // allocation
    //
    if (emptySeedPrefix) {
      // set flag used in cb for prefix verification
      usingNewSeedPrefix.store(true, std::memory_order_relaxed);

      // synchronization primitive
      waitBaton.reset();

      // clean state for node -> allocated prefix mapping
      nodeToPrefix_.withWLock(
          [&](auto& nodeToPrefix) { nodeToPrefix.clear(); });

      // announce new seed prefix
      auto prefixAllocParam = folly::sformat(
          "{},{}",
          folly::IPAddress::networkToString(newSeedPrefix),
          kAllocPrefixLen);
      evb.getEvb()->runInEventBaseThreadAndWait([&]() {
        auto res = kvStoreClient->setKey(
            kTestingAreaName,
            Constants::kSeedPrefixAllocParamKey.toString(),
            prefixAllocParam);
        EXPECT_TRUE(res.has_value());
      });

      // wait for prefix allocation to finish
      LOG(INFO) << "Waiting for full allocation to complete with new "
                << "seed prefix: " << prefixAllocParam;
      waitBaton.wait();
    }

    // close all queues
    for (auto& queue : prefixQueues) {
      queue.close();
    }
    for (auto& queue : fibRouteUpdatesQueues) {
      queue.close();
    }
    staticRouteUpdatesQueue.close();
    prefixMgrRouteUpdatesQueue.close();
    kvRequestQueue.close();
    logSampleQueue.close();
    kvStoreWrapper->closeQueue();

    kvStoreClient->stop();
    kvStoreClient.reset();

    int allocatorId = 0;
    for (auto& allocator : allocators) {
      allocator->stop();
      allocator->waitUntilStopped();
      prefixAllocatorThreads.at(allocatorId++).join();
    }

    int prefixManagerId = 0;
    for (auto& prefixManager : prefixManagers) {
      prefixManager->stop();
      prefixManager->waitUntilStopped();
      prefixManagerThreads.at(prefixManagerId++).join();
      prefixManager.reset();
    }

    kvStoreWrapper->stop();
    kvStoreWrapper.reset();

    int configStoreId = 0;
    for (auto& configStore : configStores) {
      configStore->stop();
      configStore->waitUntilStopped();
      configStoreThreads.at(configStoreId++).join();
      configStore.reset();
    }

    evb.stop();
    evb.waitUntilStopped();
    evbThread.join();

    // Verify at the end when everything is stopped
    for (uint32_t i = 0; i < numAllocators; ++i) {
      if (round == 0) {
        // save it to be compared against in round 1
        auto index = allocators[i]->getMyPrefixIndex();
        ASSERT_TRUE(index.has_value());
        lastPrefixes_.emplace_back(*index);
      } else {
        auto index = allocators[i]->getMyPrefixIndex();
        ASSERT_TRUE(index.has_value());
        EXPECT_EQ(lastPrefixes_[i], *index);
      }

      const auto myNodeName = folly::sformat("node-{}", i);
      const auto prefix = getNthPrefix(
          emptySeedPrefix ? newSeedPrefix : seedPrefix,
          kAllocPrefixLen,
          lastPrefixes_[i]);
      ASSERT_EQ(1, nodeToPrefix_->count(myNodeName));
      ASSERT_EQ(prefix, nodeToPrefix_->at(myNodeName));
    }
  } // for
}

/**
 * This test aims at testing clearing previous allocation on failure to obtain
 * allocation parameters from KvStore and ConfigStore as well as update of
 * seed prefix.
 */
TEST_F(PrefixAllocatorFixture, UpdateAllocation) {
  SetUp(thrift::PrefixAllocationMode::DYNAMIC_LEAF_NODE);

  OpenrEventBase eventBase;
  int scheduleAt{0};
  folly::Synchronized<std::optional<folly::CIDRNetwork>> allocPrefix;
  std::atomic<bool> hasAllocPrefix{false};
  const uint8_t allocPrefixLen = 24;
  const auto seedPrefix = folly::IPAddress::createNetwork("10.1.0.0/16");
  auto prefixAllocParam = folly::sformat(
      "{},{}", folly::IPAddress::networkToString(seedPrefix), allocPrefixLen);

  auto cb = [&](const std::string& prefixStr,
                std::optional<thrift::Value> val) mutable noexcept {
    // Parse PrefixDb
    // Ignore update if:
    //  1) val is std::nullopt;
    //  2) val has no value field inside `thrift::Value`(e.g. ttl update)
    if ((not val.has_value()) or (not val.value().value_ref())) {
      return;
    }

    try {
      auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
          *val.value().value_ref(), serializer);
      auto prefixes = *prefixDb.prefixEntries_ref();
      bool isPrefixDbDeleted = *prefixDb.deletePrefix_ref();

      // prefixDb marked by `deletedPrefix` to indicate prefix is stale.
      // ATTN: With per-prefix-key advertisement, `prefixEntries` will never
      //       be empty.
      EXPECT_GE(1, prefixes.size());
      if (isPrefixDbDeleted or prefixes.empty()) {
        allocPrefix.withWLock(
            [&](auto& allocatedPrefix) { allocatedPrefix = std::nullopt; });
        LOG(INFO) << "Lost allocated prefix!";
        hasAllocPrefix.store(false, std::memory_order_relaxed);
      } else {
        EXPECT_EQ(
            thrift::PrefixType::PREFIX_ALLOCATOR, *prefixes.back().type_ref());
        auto prefix = toIPNetwork(*prefixes.back().prefix_ref());
        EXPECT_EQ(allocPrefixLen, prefix.second);
        allocPrefix.withWLock(
            [&](auto& allocatedPrefix) { allocatedPrefix = prefix; });
        LOG(INFO) << "Got new prefix allocation: " << prefix.first;
        hasAllocPrefix.store(true, std::memory_order_relaxed);
      }
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to deserialize corresponding value for key "
                 << prefixStr << ". Exception: " << folly::exceptionStr(ex);
    }
  };

  eventBase.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // Set callback
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
          KvStoreFilters kvStoreFilters(
              {Constants::kPrefixDbMarker.toString()}, {myNodeName_});

          kvStoreClient_->subscribeKeyFilter(std::move(kvStoreFilters), cb);
        });

        //
        // 1) Set seed prefix in kvStore and verify that we get an elected
        // prefix
        //
        // announce new seed prefix
        hasAllocPrefix.store(false, std::memory_order_relaxed);
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
          auto res = kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kSeedPrefixAllocParamKey.toString(),
              prefixAllocParam);
          EXPECT_TRUE(res.has_value());
        });
        // busy loop until we have prefix
        while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        allocPrefix.withRLock([&](auto& allocatedPrefix) {
          EXPECT_TRUE(allocatedPrefix.has_value());
          if (allocatedPrefix.has_value()) {
            EXPECT_EQ(allocPrefixLen, allocatedPrefix->second);
            EXPECT_TRUE(allocatedPrefix->first.inSubnet("10.1.0.0/16"));
          }
        });
        LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

        //
        // 2) Clear prefix-allocator, clear prefix in config store and config
        // store and restart prefix allcoator. Expect prefix to be withdrawn
        //
        // ATTN: Clean up kvStoreUpdatesQueue before shutting down
        // prefixAllocator.
        // resetQueue() will clean up ALL readers including kvStoreClient.
        // Must recreate it to receive KvStore publication.
        prefixUpdatesQueue_.close();
        fibRouteUpdatesQueue_.close();
        kvStoreWrapper_->closeQueue();
        kvStoreClient_.reset();
        prefixAllocator_->stop();
        prefixAllocator_->waitUntilStopped();
        prefixAllocatorThread_->join();
        prefixAllocator_.reset();
        prefixManager_->stop();
        prefixManager_->waitUntilStopped();
        prefixManagerThread_->join();
        prefixManager_.reset();

        // reopen queue and restart prefixAllocator/prefixManager
        prefixUpdatesQueue_.open();
        fibRouteUpdatesQueue_.open();
        kvStoreWrapper_->openQueue();
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() noexcept {
          kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
              &evb_, myNodeName_, kvStoreWrapper_->getKvStore());
          // Set callback
          KvStoreFilters kvStoreFilters(
              {Constants::kPrefixDbMarker.toString()}, {myNodeName_});
          kvStoreClient_->subscribeKeyFilter(std::move(kvStoreFilters), cb);
          kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kSeedPrefixAllocParamKey.toString(),
              "",
              0,
              std::chrono::milliseconds(10)); // erase-key
          kvStoreClient_->unsetKey(
              kTestingAreaName, Constants::kSeedPrefixAllocParamKey.toString());
        });
        configStore_->erase("prefix-allocator-config").get();
      });

  eventBase.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // wait long enough for key to expire

        hasAllocPrefix.store(true, std::memory_order_relaxed);
        createPrefixManager();
        createPrefixAllocator();

        // Dump KeyVals from KvStore and push to kvStoreUpdatesQueue. This
        // mimics the behaviors of PrefixManager receives all KvStore KeyVals in
        // OpenR initialization process.
        auto dumpedKeyVals = kvStoreWrapper_->dumpAll(kTestingAreaName);
        kvStoreWrapper_->pushToKvStoreUpdatesQueue(
            kTestingAreaName, dumpedKeyVals);
        // Push kvStoreSynced signal to trigger initial
        // PrefixManager::syncKvStore().
        kvStoreWrapper_->publishKvStoreSynced();
      });

  eventBase.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 10 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        while (hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        // PrefixManager clears  prefixes previously advertised but not
        // allocated/originated after reboot.
        EXPECT_FALSE(allocPrefix->has_value());
        LOG(INFO) << "Step-2: Lost allocated prefix";

        //
        // 3) Set prefix and expect new prefix to be elected
        //
        hasAllocPrefix.store(false, std::memory_order_relaxed);
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() noexcept {
          auto res2 = kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kSeedPrefixAllocParamKey.toString(),
              prefixAllocParam);
          EXPECT_TRUE(res2.has_value());
        });
        while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        EXPECT_TRUE(allocPrefix->has_value());
        LOG(INFO) << "Step-3: Received allocated prefix from KvStore.";

        // cleanup
        evb_.getEvb()->runInEventBaseThreadAndWait(
            [&]() { kvStoreClient_->unsubscribeKeyFilter(); });

        eventBase.stop();
      });

  // let magic happen
  eventBase.run();
}

/**
 * The following test allocates a prefix based on the allocParams, then
 * static allocation key is inserted with the prefix that's allocated.
 * When the static allocation key is received, prefix allocator should
 * detect a collision and reallocate a new prefix.
 */
TEST_F(PrefixAllocatorFixture, StaticPrefixUpdate) {
  SetUp(thrift::PrefixAllocationMode::DYNAMIC_LEAF_NODE);

  folly::CIDRNetwork prevAllocPrefix;
  folly::Baton waitBaton;
  const uint8_t allocPrefixLen = 64;
  std::string ip6{"face:b00c:d00d::/61"};

  //
  // 1) Set seed prefix in kvStore and verify that we get an elected prefix
  //
  // announce new seed prefix
  const auto seedPrefix = folly::IPAddress::createNetwork(ip6);
  auto prefixAllocParam = folly::sformat(
      "{},{}", folly::IPAddress::networkToString(seedPrefix), allocPrefixLen);
  evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
    auto res = kvStoreClient_->setKey(
        kTestingAreaName,
        Constants::kSeedPrefixAllocParamKey.toString(),
        prefixAllocParam);
    EXPECT_TRUE(res.has_value());
  });

  evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
    // dump key with regex: "prefix:<node_name>" from KvStore
    auto expPrefixKey = folly::sformat(
        "{}{}", Constants::kPrefixDbMarker.toString(), myNodeName_);
    std::optional<thrift::KeyVals> maybeKeyVals;

    while (true) {
      try {
        thrift::KeyDumpParams params;
        params.prefix_ref() = expPrefixKey;
        params.keys_ref() = {expPrefixKey};
        auto pub = *kvStoreWrapper_->getKvStore()
                        ->semifuture_dumpKvStoreKeys(
                            std::move(params), {kTestingAreaName})
                        .get()
                        ->begin();
        maybeKeyVals = *pub.keyVals_ref();
      } catch (const std::exception& ex) {
        LOG(ERROR) << fmt::format(
            "Failed to dump keys with prefix: {}. Exception: {}",
            expPrefixKey,
            ex.what());
        maybeKeyVals = std::nullopt;
      }

      if (maybeKeyVals.has_value() and maybeKeyVals.value().size()) {
        break;
      }
      std::this_thread::yield();
    }

    // verify allocated prefix
    for (const auto& [key, rawVal] : maybeKeyVals.value()) {
      ASSERT_TRUE(rawVal.value_ref().has_value());
      auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
          rawVal.value_ref().value(), serializer);
      auto prefixes = *prefixDb.prefixEntries_ref();
      EXPECT_EQ(1, prefixes.size());
      ASSERT_FALSE(*prefixDb.deletePrefix_ref());

      auto prefix = toIPNetwork(*prefixes.back().prefix_ref());
      EXPECT_EQ(allocPrefixLen, prefix.second);
      EXPECT_TRUE(prefix.first.inSubnet(ip6));
      // record prefix for later injection
      prevAllocPrefix = prefix;
    }
  });

  LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

  // now insert e2e-network-allocation with the allocated v6 address.
  // prefix allocator subscribes to this key and should detect address
  // collision, and restart the prefix allocator to assign a different address
  thrift::StaticAllocation staticAlloc;
  staticAlloc.nodePrefixes_ref()[myNodeName_] =
      toIpPrefix(folly::IPAddress::networkToString(prevAllocPrefix));
  evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
    auto res0 = kvStoreClient_->setKey(
        kTestingAreaName,
        Constants::kStaticPrefixAllocParamKey.toString(),
        writeThriftObjStr(staticAlloc, serializer),
        1);
    EXPECT_TRUE(res0.has_value());
  });

  LOG(INFO) << "Prefix: " << folly::IPAddress::networkToString(prevAllocPrefix)
            << " has been inserted static allocation params";

  evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
    auto expPrefixKey = folly::sformat(
        "{}{}:[{}]",
        Constants::kPrefixDbMarker.toString(),
        myNodeName_,
        folly::IPAddress::networkToString(prevAllocPrefix));
    std::vector<thrift::PrefixEntry> prefixes{};

    while (true) {
      auto maybeVal = kvStoreClient_->getKey(kTestingAreaName, expPrefixKey);
      ASSERT_TRUE(maybeVal.has_value());
      ASSERT_TRUE(maybeVal.value().value_ref().has_value());
      auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
          maybeVal.value().value_ref().value(), serializer);
      prefixes = *prefixDb.prefixEntries_ref();
      if (*prefixDb.deletePrefix_ref()) {
        break;
      }
      std::this_thread::yield();
    }
    LOG(INFO)
        << "Prefix: " << folly::IPAddress::networkToString(prevAllocPrefix)
        << " has been withdrawn...";

    EXPECT_EQ(1, prefixes.size());
    auto prefix = toIPNetwork(*prefixes.back().prefix_ref());
    EXPECT_EQ(allocPrefixLen, prefix.second);
    EXPECT_TRUE(prefix.first.inSubnet(ip6));
    EXPECT_EQ(prefix, prevAllocPrefix);
  });
  LOG(INFO) << "Step-2: Received allocated prefix from KvStore.";

  // Set staticAlloc params with:
  //    face::b00c:d00d:0::/64
  //    face::b00c:d00d:1::/64
  //    face::b00c:d00d:2::/64
  //    face::b00c:d00d:3::/64
  //    face::b00c:d00d:4::/64
  //
  //    face::b00c:d00d:6::/64
  //    face::b00c:d00d:7::/64
  //
  //    Within the allocation range of `face:b00c:d00d::/61` ->
  //    `face:b00c:d00d::/64`. `face:b00c:d00d:5::/64` is the ONLY
  //    available prefix to be allocated by prefixAllocator.
  waitBaton.reset();

  auto expectedPrefix = "face:b00c:d00d:5::";
  staticAlloc.nodePrefixes_ref()["dontcare0"] =
      toIpPrefix("face:b00c:d00d:0::/64");
  staticAlloc.nodePrefixes_ref()["dontcare1"] =
      toIpPrefix("face:b00c:d00d:1::/64");
  staticAlloc.nodePrefixes_ref()["dontcare2"] =
      toIpPrefix("face:b00c:d00d:2::/64");
  staticAlloc.nodePrefixes_ref()["dontcare3"] =
      toIpPrefix("face:b00c:d00d:3::/64");
  staticAlloc.nodePrefixes_ref()["dontcare4"] =
      toIpPrefix("face:b00c:d00d:4::/64");
  staticAlloc.nodePrefixes_ref()["dontcare6"] =
      toIpPrefix("face:b00c:d00d:6::/64");
  staticAlloc.nodePrefixes_ref()["dontcare7"] =
      toIpPrefix("face:b00c:d00d:7::/64");

  evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
    auto res5 = kvStoreClient_->setKey(
        kTestingAreaName,
        Constants::kStaticPrefixAllocParamKey.toString(),
        writeThriftObjStr(staticAlloc, serializer),
        2);
    EXPECT_TRUE(res5.has_value());
  });

  // Wait for prefix allocation to update. We may timeout if the prefix index
  // is the same as expected one
  waitBaton.try_wait_for(std::chrono::seconds(2));

  // check the prefix allocated is the only available prefix
  // ATTN: DO NOT rely on global `allocPrefix`, which is populated
  //       via cb. Under per-prfix-key situation, key withdrawn/advertising
  //       can come in any order.
  evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
    auto expPrefixKey = folly::sformat(
        "{}{}:[{}/{}]",
        Constants::kPrefixDbMarker.toString(),
        myNodeName_,
        expectedPrefix,
        allocPrefixLen);
    auto maybeVal = kvStoreClient_->getKey(kTestingAreaName, expPrefixKey);
    ASSERT_TRUE(maybeVal.has_value());
    ASSERT_TRUE(maybeVal.value().value_ref().has_value());
    auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
        maybeVal.value().value_ref().value(), serializer);
    auto prefixes = *prefixDb.prefixEntries_ref();
    EXPECT_EQ(1, prefixes.size());
    ASSERT_FALSE(*prefixDb.deletePrefix_ref());

    auto prefix = toIPNetwork(*prefixes.back().prefix_ref());
    EXPECT_EQ(allocPrefixLen, prefix.second);
    EXPECT_TRUE(prefix.first.inSubnet(ip6));
    EXPECT_EQ(prefix.first.str(), expectedPrefix);
  });

  LOG(INFO) << "Step-3: Received allocated prefix from KvStore.";
}

/**
 * Tests static allocation mode of PrefixAllocator
 */
TEST_F(PrefixAllocatorFixture, StaticAllocation) {
  SetUp(thrift::PrefixAllocationMode::STATIC);
  OpenrEventBase eventBase;
  int scheduleAt{0};

  thrift::StaticAllocation staticAlloc;
  folly::Synchronized<std::optional<folly::CIDRNetwork>> allocPrefix;
  std::atomic<bool> hasAllocPrefix{false};

  auto cb = [&](const std::string& prefixStr,
                std::optional<thrift::Value> val) mutable noexcept {
    // Parse PrefixDb
    // Ignore update if:
    //  1) val is std::nullopt;
    //  2) val has no value field inside `thrift::Value`(e.g. ttl update)
    if ((not val.has_value()) or (not val.value().value_ref())) {
      return;
    }
    try {
      auto prefixDb = readThriftObjStr<thrift::PrefixDatabase>(
          *val.value().value_ref(), serializer);
      auto prefixes = *prefixDb.prefixEntries_ref();
      bool isPrefixDbDeleted = *prefixDb.deletePrefix_ref();

      // prefixDb marked by `deletedPrefix` to indicate prefix is stale.
      // ATTN: With per-prefix-key advertisement, `prefixEntries` will never
      //       be empty.
      EXPECT_GE(1, prefixes.size());
      if (isPrefixDbDeleted or prefixes.empty()) {
        allocPrefix.withWLock(
            [&](auto& allocatedPrefix) { allocatedPrefix = std::nullopt; });
        LOG(INFO) << "Lost allocated prefix!";
        hasAllocPrefix.store(false, std::memory_order_relaxed);
      } else {
        EXPECT_EQ(
            thrift::PrefixType::PREFIX_ALLOCATOR, *prefixes[0].type_ref());
        auto prefix = toIPNetwork(*prefixes[0].prefix_ref());
        allocPrefix.withWLock(
            [&](auto& allocatedPrefix) { allocatedPrefix = prefix; });
        LOG(INFO) << "Got new prefix allocation!";
        hasAllocPrefix.store(true, std::memory_order_relaxed);
      }
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to deserialize corresponding value for key "
                 << prefixStr << ". Exception: " << folly::exceptionStr(ex);
    }
  };

  eventBase.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 0), [&]() noexcept {
        // Set callback

        evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
          KvStoreFilters kvStoreFilters(
              {Constants::kPrefixDbMarker.toString()}, {myNodeName_});

          kvStoreClient_->subscribeKeyFilter(std::move(kvStoreFilters), cb);
        });

        //
        // 1) Set static allocation in KvStore
        //
        hasAllocPrefix.store(false, std::memory_order_relaxed);
        staticAlloc.nodePrefixes_ref()[myNodeName_] = toIpPrefix("1.2.3.0/24");
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
          auto res = kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kStaticPrefixAllocParamKey.toString(),
              writeThriftObjStr(staticAlloc, serializer));
          EXPECT_TRUE(res.has_value());
        });
        // busy loop until we have prefix
        while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        allocPrefix.withRLock([&](auto& allocatedPrefix) {
          EXPECT_TRUE(allocatedPrefix.has_value());
          if (allocatedPrefix.has_value()) {
            EXPECT_EQ(
                allocatedPrefix, folly::IPAddress::createNetwork("1.2.3.0/24"));
          }
        });
        LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

        //
        // 2) Stop prefix-allocator, clear prefix in config store and config
        // store and restart prefix allcoator. Expect prefix to be withdrawn
        //
        // ATTN: Clean up kvStoreUpdatesQueue before shutting down
        // prefixAllocator.
        // resetQueue() will clean up ALL readers including kvStoreClient.
        // Must recreate it to receive KvStore publication.
        prefixUpdatesQueue_.close();
        fibRouteUpdatesQueue_.close();
        kvStoreWrapper_->closeQueue();
        kvStoreClient_.reset();
        prefixAllocator_->stop();
        prefixAllocator_->waitUntilStopped();
        prefixAllocatorThread_->join();
        prefixAllocator_.reset();
        prefixManager_->stop();
        prefixManager_->waitUntilStopped();
        prefixManagerThread_->join();
        prefixManager_.reset();

        // reopen queue and restart prefixAllocator/prefixManager
        prefixUpdatesQueue_.open();
        fibRouteUpdatesQueue_.open();
        kvStoreWrapper_->openQueue();
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() noexcept {
          kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
              &evb_, myNodeName_, kvStoreWrapper_->getKvStore());
          // Set callback
          KvStoreFilters kvStoreFilters(
              {Constants::kPrefixDbMarker.toString()}, {myNodeName_});
          kvStoreClient_->subscribeKeyFilter(std::move(kvStoreFilters), cb);
          kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kStaticPrefixAllocParamKey.toString(),
              "",
              0,
              std::chrono::milliseconds(10)); // erase-key
          kvStoreClient_->unsetKey(
              kTestingAreaName,
              Constants::kStaticPrefixAllocParamKey.toString());
        });
        configStore_->erase("prefix-allocator-config").get();
      });

  eventBase.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // wait long enough for key to expire

        hasAllocPrefix.store(true, std::memory_order_relaxed);
        createPrefixManager();
        createPrefixAllocator();

        // Dump KeyVals from KvStore and push to kvStoreUpdatesQueue. This
        // mimics the behaviors of PrefixManager receives all KvStore KeyVals in
        // OpenR initialization process.
        auto dumpedKeyVals = kvStoreWrapper_->dumpAll(kTestingAreaName);
        kvStoreWrapper_->pushToKvStoreUpdatesQueue(
            kTestingAreaName, dumpedKeyVals);
        // Push kvStoreSynced signal to trigger initial
        // PrefixManager::syncKvStore().
        kvStoreWrapper_->publishKvStoreSynced();
      });

  eventBase.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 10 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // wait long enough for key to expire
        while (hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        // PrefixManager clears  prefixes previously advertised but not
        // allocated/originated after reboot.
        EXPECT_FALSE(allocPrefix->has_value());
        LOG(INFO) << "Step-2: Lost allocated prefix";

        //
        // 3) Set prefix and expect new prefix to be advertised
        //
        hasAllocPrefix.store(false, std::memory_order_relaxed);
        staticAlloc.nodePrefixes_ref()[myNodeName_] = toIpPrefix("3.2.1.0/24");
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
          auto res2 = kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kStaticPrefixAllocParamKey.toString(),
              writeThriftObjStr(staticAlloc, serializer));
          EXPECT_TRUE(res2.has_value());
        });
        while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        allocPrefix.withRLock([&](auto& allocatedPrefix) {
          EXPECT_TRUE(allocatedPrefix.has_value());
          if (allocatedPrefix.has_value()) {
            EXPECT_EQ(
                allocatedPrefix, folly::IPAddress::createNetwork("3.2.1.0/24"));
          }
        });
        LOG(INFO) << "Step-3: Received allocated prefix from KvStore.";

        //
        // 4) Change prefix in static config and expect the announcement
        //
        hasAllocPrefix.store(false, std::memory_order_relaxed);
        staticAlloc.nodePrefixes_ref()[myNodeName_] = toIpPrefix("5.6.7.0/24");
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
          auto res3 = kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kStaticPrefixAllocParamKey.toString(),
              writeThriftObjStr(staticAlloc, serializer));
          EXPECT_TRUE(res3.has_value());
        });
        while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        allocPrefix.withRLock([&](auto& allocatedPrefix) {
          EXPECT_TRUE(allocatedPrefix.has_value());
          if (allocatedPrefix.has_value()) {
            EXPECT_EQ(
                allocatedPrefix, folly::IPAddress::createNetwork("5.6.7.0/24"));
          }
        });
        LOG(INFO) << "Step-4: Received updated allocated prefix from KvStore.";

        //
        // 5) Remove prefix in static config and expect the withdrawal
        //
        hasAllocPrefix.store(true, std::memory_order_relaxed);
        staticAlloc.nodePrefixes_ref()->erase(myNodeName_);
        evb_.getEvb()->runInEventBaseThreadAndWait([&]() {
          auto res4 = kvStoreClient_->setKey(
              kTestingAreaName,
              Constants::kStaticPrefixAllocParamKey.toString(),
              writeThriftObjStr(staticAlloc, serializer));
          EXPECT_TRUE(res4.has_value());
        });
        while (hasAllocPrefix.load(std::memory_order_relaxed)) {
          std::this_thread::yield();
        }
        EXPECT_FALSE(allocPrefix->has_value());
        LOG(INFO)
            << "Step-5: Received withdraw for allocated prefix from KvStore.";

        // cleanup
        evb_.getEvb()->runInEventBaseThreadAndWait(
            [&]() { kvStoreClient_->unsubscribeKeyFilter(); });

        eventBase.stop();
      });

  // let magic happen
  eventBase.run();
}

TEST_F(PrefixAllocatorFixture, SyncIfaceAddresses) {
  SetUp(thrift::PrefixAllocationMode::STATIC);

  const auto ifAddr = fbnl::utils::createIfAddress(1, "192.168.0.3/31");
  const auto ifPrefix = toIpPrefix(ifAddr.getPrefix().value());
  const auto ifAddr1 =
      fbnl::utils::createIfAddress(1, "192.168.1.3/31"); // v4 global
  const auto ifAddr2 =
      fbnl::utils::createIfAddress(1, "192.168.2.3/31"); // v4 global
  const auto ifAddr3 =
      fbnl::utils::createIfAddress(1, "192.168.3.3/31"); // v4 global
  const auto ifAddr4 =
      fbnl::utils::createIfAddress(1, "127.0.0.1/32"); // v4 host
  const auto ifAddr11 =
      fbnl::utils::createIfAddress(1, "fc00::3/127"); // v6 global

  // Add link eth0
  EXPECT_EQ(0, nlSock_->addLink(fbnl::utils::createLink(1, "eth0")).get());

  // Add addr2, addr3 and addr11 in nlSock
  EXPECT_EQ(0, nlSock_->addIfAddress(ifAddr2).get());
  EXPECT_EQ(0, nlSock_->addIfAddress(ifAddr3).get());
  EXPECT_EQ(0, nlSock_->addIfAddress(ifAddr4).get());
  EXPECT_EQ(0, nlSock_->addIfAddress(ifAddr11).get());

  // Sync addr1 and addr2 for AF_INET family
  {
    std::vector<folly::CIDRNetwork> networks{
        ifAddr1.getPrefix().value(), ifAddr2.getPrefix().value()};
    auto retval = prefixAllocator_->semifuture_syncIfAddrs(
        "eth0", AF_INET, RT_SCOPE_UNIVERSE, std::move(networks));
    EXPECT_NO_THROW(std::move(retval).get());
  }

  // Verify that addr1 is added and addr3 no longer exists. In fake
  // implementation addrs are returned in the order they're added.
  {
    auto addrs = nlSock_->getAllIfAddresses().get().value();
    ASSERT_EQ(4, addrs.size());
    EXPECT_EQ(ifAddr2, addrs.at(0));
    EXPECT_EQ(ifAddr4, addrs.at(1));
    EXPECT_EQ(ifAddr11, addrs.at(2));
    EXPECT_EQ(ifAddr1, addrs.at(3));
  }
}

TEST_F(PrefixAllocatorFixture, AddRemoveIfAddresses) {
  SetUp(thrift::PrefixAllocationMode::STATIC);

  const auto ifAddr = fbnl::utils::createIfAddress(1, "192.168.0.3/31");
  const auto network = ifAddr.getPrefix().value();

  // Add link eth0
  EXPECT_EQ(0, nlSock_->addLink(fbnl::utils::createLink(1, "eth0")).get());

  // Add address on eth0 and verify
  {
    auto retval = prefixAllocator_->semifuture_addRemoveIfAddr(
        true, std::string("eth0"), {network});
    EXPECT_NO_THROW(std::move(retval).get());
    auto addrs = nlSock_->getAllIfAddresses().get().value();
    ASSERT_EQ(1, addrs.size());
    EXPECT_EQ(ifAddr, addrs.at(0));
  }

  {
    auto retval = prefixAllocator_->semifuture_getIfAddrs(
        std::string("eth0"), AF_INET, RT_SCOPE_UNIVERSE);
    auto addrs = std::move(retval).get();
    ASSERT_EQ(1, addrs.size());
    EXPECT_EQ(network, addrs.at(0));
  }

  // Remove address from eth0 and verify
  {
    auto retval = prefixAllocator_->semifuture_addRemoveIfAddr(
        false, std::string("eth0"), {network});
    EXPECT_NO_THROW(std::move(retval).get());
    auto addrs = nlSock_->getAllIfAddresses().get().value();
    EXPECT_EQ(0, addrs.size());
  }
}

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
  { EXPECT_ANY_THROW(auto p = PrefixAllocator::parseParamsStr("face::,64")); }

  // Incorrect seed prefix
  {
    EXPECT_ANY_THROW(
        auto p = PrefixAllocator::parseParamsStr("face::b00c::/56,64"));
  }

  // Seed prefix same or greather than alloc prefix length (error case).
  {
    EXPECT_ANY_THROW(
        auto p = PrefixAllocator::parseParamsStr("face:b00c::/64,64"));
    EXPECT_ANY_THROW(PrefixAllocator::parseParamsStr("face:b00c::/74,64"));
  }

  // Correct case - v6
  {
    auto p = PrefixAllocator::parseParamsStr("face::/56,64");
    EXPECT_EQ(folly::IPAddress::createNetwork("face::/56"), p.first);
    EXPECT_EQ(64, p.second);
  }

  // Correct case - v4
  {
    // Note: last byte will be masked off
    auto p = PrefixAllocator::parseParamsStr("1.2.0.1/16,24");
    EXPECT_EQ(folly::IPAddress::createNetwork("1.2.0.0/16"), p.first);
    EXPECT_EQ(24, p.second);
  }
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
