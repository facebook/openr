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
#include <folly/init/Init.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/gen/Base.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/kvstore/KvStoreWrapper.h>

#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/util/ScopedServerThread.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/BinaryProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

using namespace std;
using namespace folly;
using namespace folly::gen;
using namespace openr;

using apache::thrift::FRAGILE;
using namespace std::chrono_literals;

DEFINE_int32(seed_prefix_len, 125, "length of seed prefix");

// interval for periodic syncs
const std::chrono::milliseconds kSyncInterval(10);

// key marker for allocating prefix
const AllocPrefixMarker kAllocPrefixMarker{"allocprefix:"};

// length of allocated prefix
const int kAllocPrefixLen = 128;

// key used by seed prefix in kvstore
const string kConfigStoreUrl{"inproc://openr_config_store_cmd"};

class PrefixAllocatorFixture : public ::testing::TestWithParam<bool> {
 public:
  void
  SetUp() override {
    // threadID constant
    const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

    //
    // Start KvStore and attach a client to it
    //
    kvStoreWrapper_ = std::make_unique<KvStoreWrapper>(
        zmqContext_,
        myNodeName_,
        std::chrono::seconds(60) /* db sync interval */,
        std::chrono::seconds(600) /* counter submit interval */,
        std::unordered_map<std::string, openr::thrift::PeerSpec>{});
    kvStoreWrapper_->run();
    kvStoreClient_ = std::make_unique<KvStoreClient>(
        zmqContext_,
        &evl_,
        myNodeName_,
        kvStoreWrapper_->localCmdUrl,
        kvStoreWrapper_->localPubUrl);

    LOG(INFO) << "The test store is running";

    //
    // Start persistent config store
    //
    tempFileName_ = folly::sformat("/tmp/openr.{}", tid);
    configStore_ = std::make_unique<PersistentStore>(
        tempFileName_,
        PersistentStoreUrl{kConfigStoreUrl},
        zmqContext_);
    threads_.emplace_back([&]() noexcept { configStore_->run(); });
    configStore_->waitUntilRunning();
    configStoreClient_ = std::make_unique<PersistentStoreClient>(
        PersistentStoreUrl{kConfigStoreUrl},
        zmqContext_);

    // Erase previous configs (if any)
    configStoreClient_->erase("prefix-allocator-config");
    configStoreClient_->erase("prefix-manager-config");

    mockServiceHandler_ = std::make_shared<MockSystemServiceHandler>();
    server_ = std::make_shared<apache::thrift::ThriftServer>();
    server_->setPort(0);
    server_->setInterface(mockServiceHandler_);

    systemThriftThread_.start(server_);
    port_ = systemThriftThread_.getAddress()->getPort();

    //
    // Start PrefixManager
    //
    prefixManager_ = std::make_unique<PrefixManager>(
        myNodeName_,
        PrefixManagerGlobalCmdUrl{pfxMgrGlobalUrl_},
        PrefixManagerLocalCmdUrl{pfxMgrLocalUrl_},
        PersistentStoreUrl{kConfigStoreUrl},
        KvStoreLocalCmdUrl{kvStoreWrapper_->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper_->localPubUrl},
        PrefixDbMarker{"prefix:"},
        false /* prefix-manager perf measurement */,
        MonitorSubmitUrl{"inproc://monitor_submit"},
        zmqContext_);
    threads_.emplace_back([&]() noexcept { prefixManager_->run(); });
    prefixManager_->waitUntilRunning();

    //
    // Create PrefixAllocator
    //
    createPrefixAllocator();
  }

  //
  // (Re)Create PrefixAllocator
  void createPrefixAllocator() {
    // Destroy one if running
    if (prefixAllocator_) {
      prefixAllocator_->stop();
      prefixAllocator_->waitUntilStopped();
      prefixAllocator_.reset();
    }

    prefixAllocator_ = make_unique<PrefixAllocator>(
        myNodeName_,
        KvStoreLocalCmdUrl{kvStoreWrapper_->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper_->localPubUrl},
        PrefixManagerLocalCmdUrl{pfxMgrLocalUrl_},
        MonitorSubmitUrl{"inproc://monitor_submit"},
        kAllocPrefixMarker,
        GetParam()
          ? PrefixAllocatorMode(PrefixAllocatorModeStatic())
          : PrefixAllocatorMode(PrefixAllocatorModeSeeded()),
        false /* set loopback addr */,
        false /* override global address */,
        "" /* loopback interface name */,
        kSyncInterval,
        PersistentStoreUrl{kConfigStoreUrl},
        zmqContext_,
        port_);
    threads_.emplace_back([&]() noexcept { prefixAllocator_->run(); });
    prefixAllocator_->waitUntilRunning();
  }

  void
  TearDown() override {
    kvStoreClient_.reset();
    configStoreClient_.reset();

    // Stop various modules
    prefixAllocator_->stop();
    prefixAllocator_->waitUntilStopped();
    prefixManager_->stop();
    prefixManager_->waitUntilStopped();
    configStore_->stop();
    configStore_->waitUntilStopped();
    kvStoreWrapper_->stop();

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

  // Main thread event loop
  fbzmq::ZmqEventLoop evl_;

  const std::string myNodeName_{"test-node"};
  const std::string pfxMgrGlobalUrl_{"inproc://prefix-manager-global"};
  const std::string pfxMgrLocalUrl_{"inproc://prefix-manager-local-{}"};
  std::string tempFileName_;

  std::unique_ptr<KvStoreWrapper> kvStoreWrapper_;
  std::unique_ptr<KvStoreClient> kvStoreClient_;
  std::unique_ptr<PersistentStore> configStore_;
  std::unique_ptr<PersistentStoreClient> configStoreClient_;
  std::unique_ptr<PrefixManager> prefixManager_;
  std::unique_ptr<PrefixAllocator> prefixAllocator_;

  std::vector<std::thread> threads_;

  // create serializer object for parsing kvstore key/values
  apache::thrift::CompactSerializer serializer;

  std::shared_ptr<MockSystemServiceHandler> mockServiceHandler_;
  int32_t port_{0};
  std::shared_ptr<apache::thrift::ThriftServer> server_;
  apache::thrift::util::ScopedServerThread systemThriftThread_;
};

class PrefixAllocTest : public ::testing::TestWithParam<bool> {
 public:
   void SetUp() override {
    mockServiceHandler_ = std::make_shared<MockSystemServiceHandler>();
    server_ = std::make_shared<apache::thrift::ThriftServer>();
    server_->setPort(0);
    server_->setInterface(mockServiceHandler_);

    systemThriftThread_.start(server_);
    port_ = systemThriftThread_.getAddress()->getPort();
  }

  void TearDown() override {
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

  folly::Optional<PrefixAllocatorParams> maybeAllocParams;
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
    // Create another ZmqEventLoop instance for looping clients
    // Put in outer scope of kvstore client to ensure it's still alive when the
    // client is destroyed
    fbzmq::ZmqEventLoop evl;
    std::atomic<bool> shouldWait{true};
    std::atomic<bool> usingNewSeedPrefix{false};

    vector<std::unique_ptr<PersistentStore>> configStores;
    vector<std::unique_ptr<PrefixManager>> prefixManagers;
    vector<std::unique_ptr<PrefixAllocator>> allocators;
    vector<thread> threads;

    // needed while allocators manipulate test state
    folly::Synchronized<std::unordered_map<
        std::string /* nodeId */,
        folly::CIDRNetwork /* seed prefix */>>
        nodeToPrefix;
    auto prefixDbCb = [&](
        std::string const& /* key */,
        folly::Optional<thrift::Value> value) mutable noexcept {
      // Parse PrefixDb
      ASSERT_TRUE(value.hasValue());
      ASSERT_TRUE(value.value().value.hasValue());
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value.value().value.value(), serializer);
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
    // 1) spin up a kvstore and create KvStoreClient
    //

    const auto nodeId = folly::sformat("test_store{}", round);
    auto store = std::make_shared<KvStoreWrapper>(
        zmqContext,
        nodeId,
        std::chrono::seconds(60) /* db sync interval */,
        std::chrono::seconds(600) /* counter submit interval */,
        std::unordered_map<std::string, openr::thrift::PeerSpec>{});
    store->run();
    LOG(INFO) << "The test store is running";

    // Attach a kvstore client in main event loop
    auto kvStoreClient = std::make_unique<KvStoreClient>(
        zmqContext, &evl, nodeId, store->localCmdUrl, store->localPubUrl);

    // Set seed prefix in KvStore
    if (emptySeedPrefix) {
      // inject seed prefix
      auto prefixAllocParam = folly::sformat(
          "{},{}",
          folly::IPAddress::networkToString(seedPrefix),
          kAllocPrefixLen);
      auto res = kvStoreClient->setKey(
          Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
      EXPECT_FALSE(res.hasError()) << res.error();
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
      kvStoreClient->subscribeKey(sformat("prefix:{}", myNodeName),
                      prefixDbCb, false);

      // get a unique temp file name
      auto tempFileName = folly::sformat("/tmp/openr.{}.{}", tid, i);
      tempFileNames.emplace_back(tempFileName);

      // spin up config store server for this allocator
      auto configStore = std::make_unique<PersistentStore>(
          tempFileName,
          PersistentStoreUrl{kConfigStoreUrl + myNodeName},
          zmqContext);
      threads.emplace_back([&configStore]() noexcept { configStore->run(); });
      configStore->waitUntilRunning();
      configStores.emplace_back(std::move(configStore));

      // Temporary config store for PrefixManager so that they are not being
      // used
      tempFileName = folly::sformat("/tmp/openr.{}.{}.{}", tid, round, i);
      tempFileNames.emplace_back(tempFileName);
      auto tempConfigStore = std::make_unique<PersistentStore>(
          tempFileName,
          PersistentStoreUrl{kConfigStoreUrl + myNodeName + "temp"},
          zmqContext);
      threads.emplace_back([&tempConfigStore]() noexcept {
        tempConfigStore->run();
      });
      tempConfigStore->waitUntilRunning();
      configStores.emplace_back(std::move(tempConfigStore));

      // spin up prefix manager
      const auto pfxMgrGlobalUrl =
          folly::sformat("inproc://prefix-manager-global-{}", myNodeName);
      const auto pfxMgrLocalUrl =
          folly::sformat("inproc://prefix-manager-local-{}", myNodeName);
      auto prefixManager = std::make_unique<PrefixManager>(
          myNodeName,
          PrefixManagerGlobalCmdUrl{pfxMgrGlobalUrl},
          PrefixManagerLocalCmdUrl{pfxMgrLocalUrl},
          PersistentStoreUrl{kConfigStoreUrl + myNodeName + "temp"},
          KvStoreLocalCmdUrl{store->localCmdUrl},
          KvStoreLocalPubUrl{store->localPubUrl},
          PrefixDbMarker{"prefix:"},
          false /* prefix-manager perf measurement */,
          MonitorSubmitUrl{"inproc://monitor_submit"},
          zmqContext);
      threads.emplace_back([&prefixManager]() noexcept {
        prefixManager->run();
      });
      prefixManager->waitUntilRunning();
      prefixManagers.emplace_back(std::move(prefixManager));

      auto allocator = make_unique<PrefixAllocator>(
          myNodeName,
          KvStoreLocalCmdUrl{store->localCmdUrl},
          KvStoreLocalPubUrl{store->localPubUrl},
          PrefixManagerLocalCmdUrl{pfxMgrLocalUrl},
          MonitorSubmitUrl{"inproc://monitor_submit"},
          kAllocPrefixMarker,
          maybeAllocParams.hasValue()
            ? PrefixAllocatorMode(*maybeAllocParams)
            : PrefixAllocatorMode(PrefixAllocatorModeSeeded()),
          false /* set loopback addr */,
          false /* override global address */,
          "" /* loopback interface name */,
          kSyncInterval,
          PersistentStoreUrl{kConfigStoreUrl + myNodeName},
          zmqContext,
          port_);
      threads.emplace_back([&allocator]() noexcept { allocator->run(); });
      allocator->waitUntilRunning();
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
      EXPECT_FALSE(res.hasError()) << res.error();

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
    evl.stop();
    evl.waitUntilStopped();

    LOG(INFO) << "Stop allocators";
    for (auto& allocator : allocators) {
      allocator->stop();
      allocator->waitUntilStopped();
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
        ASSERT_TRUE(index.hasValue());
        lastPrefixes.emplace_back(*index);
      } else {
        auto index = allocators[i]->getMyPrefixIndex();
        ASSERT_TRUE(index.hasValue());
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

  folly::Synchronized<folly::Optional<folly::CIDRNetwork>> allocPrefix;
  std::atomic<bool> hasAllocPrefix{false};
  const uint8_t allocPrefixLen = 24;
  const std::string subscriptionKey = folly::sformat(
      "{}{}", openr::Constants::kPrefixDbMarker.toString(), myNodeName_);

  // Set callback
  kvStoreClient_->subscribeKey(subscriptionKey,
    [&](const std::string& /* key */,
        folly::Optional<thrift::Value> value) {
      // Parse PrefixDb
      ASSERT_TRUE(value.hasValue());
      ASSERT_TRUE(value.value().value.hasValue());
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value.value().value.value(), serializer);
      auto& prefixes = prefixDb.prefixEntries;

      // Verify some expectations
      EXPECT_GE(1, prefixes.size());
      if (prefixes.empty()) {
        SYNCHRONIZED(allocPrefix) {
          allocPrefix = folly::none;
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
    },// callback
    false);

  // Start main event loop in a new thread
  threads_.emplace_back([&]() noexcept { evl_.run(); });

  //
  // 1) Set seed prefix in kvStore and verify that we get an elected prefix
  //
  // announce new seed prefix
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  const auto seedPrefix = folly::IPAddress::createNetwork("10.1.0.0/16");
  auto prefixAllocParam = folly::sformat(
      "{},{}",
      folly::IPAddress::networkToString(seedPrefix),
      allocPrefixLen);
  auto res = kvStoreClient_->setKey(
      Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
  EXPECT_FALSE(res.hasError()) << res.error();
  // busy loop until we have prefix
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.hasValue());
    if (allocPrefix.hasValue()) {
      EXPECT_EQ(allocPrefixLen, allocPrefix->second);
      EXPECT_TRUE(allocPrefix->first.inSubnet("10.1.0.0/16"));
    }
  }
  LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

  //
  // 2) Clear prefix-allocator, clear prefix in config store and config store
  // and restart prefix allcoator. Expect prefix to be withdrawn
  //
  prefixAllocator_->stop();
  prefixAllocator_->waitUntilStopped();
  prefixAllocator_.reset();
  evl_.runInEventLoop([&]() noexcept {
    kvStoreClient_->setKey(
        Constants::kSeedPrefixAllocParamKey.toString(),
        "", 0, std::chrono::milliseconds(10));   // erase-key
    kvStoreClient_->unsetKey(Constants::kSeedPrefixAllocParamKey.toString());
  });
  configStoreClient_->erase("prefix-allocator-config");
  // wait long enough for key to expire
  // @lint-ignore HOWTOEVEN1
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  hasAllocPrefix.store(true, std::memory_order_relaxed);
  createPrefixAllocator();
  while (hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(allocPrefix->hasValue());
  LOG(INFO) << "Step-2: Lost allocated prefix";

  //
  // 3) Set prefix and expect new prefix to be elected
  //
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  auto res2 = kvStoreClient_->setKey(
      Constants::kSeedPrefixAllocParamKey.toString(), prefixAllocParam);
  EXPECT_FALSE(res2.hasError()) << res2.error();
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(allocPrefix->hasValue());
  LOG(INFO) << "Step-3: Received allocated prefix from KvStore.";

  // Stop main eventloop
  evl_.stop();
  evl_.waitUntilStopped();
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
  folly::Synchronized<folly::Optional<folly::CIDRNetwork>> allocPrefix;
  std::atomic<bool> hasAllocPrefix{false};
  const std::string subscriptionKey = folly::sformat(
      "{}{}", openr::Constants::kPrefixDbMarker, myNodeName_);

  // Set callback
  kvStoreClient_->subscribeKey(subscriptionKey,
    [&](const std::string& /* key */,
        folly::Optional<thrift::Value> value) {
      // Parse PrefixDb
      ASSERT_TRUE(value.hasValue());
      ASSERT_TRUE(value.value().value.hasValue());
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value.value().value.value(), serializer);
      auto& prefixes = prefixDb.prefixEntries;

      // Verify some expectations
      EXPECT_GE(1, prefixes.size());
      if (prefixes.empty()) {
        SYNCHRONIZED(allocPrefix) {
          allocPrefix = folly::none;
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
      } // if
    },// callback
    false);

  // Start main event loop in a new thread
  threads_.emplace_back([&]() noexcept { evl_.run(); });

  //
  // 1) Set static allocation in KvStore
  //
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  staticAlloc.nodePrefixes[myNodeName_] = toIpPrefix("1.2.3.0/24");
  auto res = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer));
  EXPECT_FALSE(res.hasError()) << res.error();
  // busy loop until we have prefix
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.hasValue());
    if (allocPrefix.hasValue()) {
      EXPECT_EQ(allocPrefix, folly::IPAddress::createNetwork("1.2.3.0/24"));
    }
  }
  LOG(INFO) << "Step-1: Received allocated prefix from KvStore.";

  //
  // 2) Stop prefix-allocator, clear prefix in config store and config store
  //    and restart prefix allcoator. Expect prefix to be withdrawn
  //
  prefixAllocator_->stop();
  prefixAllocator_->waitUntilStopped();
  prefixAllocator_.reset();
  evl_.runInEventLoop([&]() noexcept {
    kvStoreClient_->setKey(
        Constants::kStaticPrefixAllocParamKey.toString(),
        "", 0, std::chrono::milliseconds(10));   // erase-key
    kvStoreClient_->unsetKey(Constants::kStaticPrefixAllocParamKey.toString());
  });
  configStoreClient_->erase("prefix-allocator-config");
  // wait long enough for key to expire
  // @lint-ignore HOWTOEVEN1
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  hasAllocPrefix.store(true, std::memory_order_relaxed);
  createPrefixAllocator();
  while (hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(allocPrefix->hasValue());
  LOG(INFO) << "Step-2: Lost allocated prefix";

  //
  // 3) Set prefix and expect new prefix to be advertised
  //
  hasAllocPrefix.store(false, std::memory_order_relaxed);
  staticAlloc.nodePrefixes[myNodeName_] = toIpPrefix("3.2.1.0/24");
  auto res2 = kvStoreClient_->setKey(
      Constants::kStaticPrefixAllocParamKey.toString(),
      fbzmq::util::writeThriftObjStr(staticAlloc, serializer));
  EXPECT_FALSE(res2.hasError()) << res2.error();
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.hasValue());
    if (allocPrefix.hasValue()) {
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
  EXPECT_FALSE(res3.hasError()) << res3.error();
  while (not hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  SYNCHRONIZED(allocPrefix) {
    EXPECT_TRUE(allocPrefix.hasValue());
    if (allocPrefix.hasValue()) {
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
  EXPECT_FALSE(res4.hasError()) << res4.error();
  while (hasAllocPrefix.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(allocPrefix->hasValue());
  LOG(INFO) << "Step-5: Received withdraw for allocated prefix from KvStore.";

  // Stop main eventloop
  evl_.stop();
  evl_.waitUntilStopped();
}

INSTANTIATE_TEST_CASE_P(
    FixtureTest, PrefixAllocatorFixture, ::testing::Bool());

TEST(PrefixAllocator, getPrefixCount) {
  {
    auto params = std::make_pair(
        folly::IPAddress::createNetwork("face::/56"), 64);
    EXPECT_EQ(256, PrefixAllocator::getPrefixCount(params));
  }
  {
    auto params = std::make_pair(
        folly::IPAddress::createNetwork("face::/64"), 64);
    EXPECT_EQ(1, PrefixAllocator::getPrefixCount(params));
  }
  {
    auto params = std::make_pair(
        folly::IPAddress::createNetwork("face::/16"), 64);
    EXPECT_EQ(1 << 31, PrefixAllocator::getPrefixCount(params));
  }
  {
    auto params = std::make_pair(
        folly::IPAddress::createNetwork("1.2.0.0/16"), 24);
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
