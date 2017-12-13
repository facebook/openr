/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <atomic>
#include <mutex>

#include <fbzmq/zmq/Common.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/gen/Base.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/kvstore/KvStoreWrapper.h>

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
const string kSeedPrefixKey = "e2e-network-prefix";
const string kConfigStoreUrlBase{"inproc://config_store_cmd"};

class PrefixAllocTest : public ::testing::TestWithParam<bool> {};

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
  folly::Optional<folly::CIDRNetwork> maybeSeedPrefix;
  if (!emptySeedPrefix) {
    maybeSeedPrefix = seedPrefix;
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
        thrift::Value const& value) mutable noexcept {
      // Parse PrefixDb
      ASSERT_TRUE(value.value.hasValue());
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          value.value.value(), serializer);
      auto& prefixes = prefixDb.prefixEntries;

      // Verify some expectations
      EXPECT_GE(1, prefixes.size());
      if (prefixes.size()) {
        EXPECT_EQ(thrift::PrefixType::PREFIX_ALLOCATOR, prefixes[0].type);
        auto prefix = toIPNetwork(prefixes[0].prefix);
        EXPECT_EQ(kAllocPrefixLen, prefix.second);
        EXPECT_TRUE(prefix.first.inSubnet(seedPrefix.first, seedPrefix.second));

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
      auto res = kvStoreClient->setKey(kSeedPrefixKey, prefixAllocParam);
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
      kvStoreClient->subscribeKey(sformat("prefix:{}", myNodeName), prefixDbCb);

      // get a unique temp file name
      auto tempFileName = folly::sformat("/tmp/openr.{}.{}", tid, i);
      tempFileNames.emplace_back(tempFileName);

      // spin up config store server for this allocator
      auto configStore = std::make_unique<PersistentStore>(
          tempFileName,
          PersistentStoreUrl{kConfigStoreUrlBase + myNodeName},
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
          PersistentStoreUrl{kConfigStoreUrlBase + myNodeName + "temp"},
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
          PersistentStoreUrl{kConfigStoreUrlBase + myNodeName + "temp"},
          KvStoreLocalCmdUrl{store->localCmdUrl},
          KvStoreLocalPubUrl{store->localPubUrl},
          folly::none,
          PrefixDbMarker{"prefix:"},
          false /* prefix-manager perf measurement */,
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
          maybeSeedPrefix,
          kAllocPrefixLen,
          false /* set loopback addr */,
          false /* override global address */,
          "" /* loopback interface name */,
          kSyncInterval,
          PersistentStoreUrl{kConfigStoreUrlBase + myNodeName},
          zmqContext);
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
      const auto prefix =
          getNthPrefix(seedPrefix, kAllocPrefixLen, lastPrefixes[i], true);
      ASSERT_EQ(1, nodeToPrefix->count(myNodeName));
      ASSERT_EQ(prefix, nodeToPrefix->at(myNodeName));
    }
  } // for
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
