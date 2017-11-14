/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include <folly/gen/Base.h>
#include <folly/gen/String.h>
#include <gtest/gtest.h>
#include <sodium.h>

#include <openr/allocators/RangeAllocator.h>
#include <openr/kvstore/KvStoreWrapper.h>

using folly::make_optional;

using namespace folly::gen;

namespace openr {

namespace {

// the size of the value string
const uint32_t kValueStrSize = 64;

// interval for periodic syncs
const std::chrono::seconds kDbSyncInterval(1);
const std::chrono::seconds kMonitorSubmitInterval(3600);

// Count for testing purpose
const uint32_t kNumStores = 3; // Total number of KvStore
const uint32_t kNumClients = 99; // Total number of KvStoreClient
} // namespace

/**
 * Base class for all of our unit-tests. Internally it has linear topology of
 * kNumStores KvStores, total of kNumClients KvStoreClients evenly distributed
 * among these KvStores.
 *
 * All of our tests create kNumClients allocators (one attached with each
 * KvStoreClient).
 */
class RangeAllocatorFixture : public ::testing::TestWithParam<bool> {
 public:
  /**
   * Create linear topology of KvStores for data flow. All of our unittests
   * for range allocations are based on this linear topology.
   */
  void
  SetUp() override {
    const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers;

    for (uint32_t i = 0; i < kNumStores; i++) {
      auto store = std::make_unique<KvStoreWrapper>(
          zmqContext,
          folly::sformat("store{}", i + 1),
          kDbSyncInterval,
          kMonitorSubmitInterval,
          emptyPeers);
      stores.emplace_back(std::move(store));
      stores.back()->run();
    }

    for (uint32_t i = 1; i < kNumStores; i++) {
      stores[i - 1]->addPeer(stores[i]->nodeId, stores[i]->getPeerSpec());
      stores[i]->addPeer(stores[i - 1]->nodeId, stores[i - 1]->getPeerSpec());
    }

    for (uint32_t i = 0; i < kNumClients; i++) {
      auto const& store = stores[i % kNumStores];
      auto client = std::make_unique<KvStoreClient>(
          zmqContext,
          &eventLoop,
          createClientName(i),
          store->localCmdUrl,
          store->localPubUrl);
      clients.emplace_back(std::move(client));
    }
  }

  void
  TearDown() override {
    clients.clear();
    for (auto& store : stores) {
      store->stop();
    }
  }

  static std::string
  createClientName(int id) {
    CHECK_LT(kNumClients, 100) << "Clients must be in 2 digits.";
    return folly::sformat("client-{}{}", id / 10, id % 10);
  }

  template <typename T>
  std::vector<std::unique_ptr<RangeAllocator<T>>>
  createAllocators(
      const std::pair<T, T>& allocRange,
      const folly::Optional<std::vector<T>> maybeInitVals,
      std::function<void(int /* client id */, folly::Optional<T>)> callback) {
    // sanity check
    if (maybeInitVals) {
      CHECK_EQ(clients.size(), maybeInitVals->size());
    }

    using namespace std::chrono_literals;
    std::vector<std::unique_ptr<RangeAllocator<T>>> allocators;
    for (size_t i = 0; i < clients.size(); i++) {
      auto allocator = std::make_unique<RangeAllocator<T>>(
          createClientName(i),
          "value:",
          clients[i].get(),
          [callback, i](folly::Optional<T> newVal) noexcept {
            callback(i, newVal);
          },
          10ms /* min backoff */,
          100ms /* max backoff */,
          overrideOwner /* override allowed */);
      // start allocator
      allocator->startAllocator(
          allocRange,
          maybeInitVals ? make_optional(maybeInitVals->at(i)) : folly::none);
      allocators.emplace_back(std::move(allocator));
    }

    return std::move(allocators);
  }

  // ZMQ Context for IO processing
  fbzmq::Context zmqContext;

  // Linear topology of stores. i <--> i+1 <--> i+2 ......
  std::vector<std::unique_ptr<KvStoreWrapper>> stores;

  // Client `i` connects to store `i % stores.size()`
  // All clients loop in same event-loop
  std::vector<std::unique_ptr<KvStoreClient>> clients;

  // ZmqEventLoop for all clients
  fbzmq::ZmqEventLoop eventLoop;

  // owner override allowed
  const bool overrideOwner = GetParam();
};

INSTANTIATE_TEST_CASE_P(
    RangeAllocatorInstance, RangeAllocatorFixture, ::testing::Bool());

/**
 * Run each allocator with distinct seed and make sure that they allocate
 * their seed value.
 *
 * Bail out when you receive kNumClients replies.
 */
TEST_P(RangeAllocatorFixture, DistinctSeed) {
  const uint32_t start = 61;
  const auto initVals =
      range(start, start + kNumClients) | as<std::vector<uint32_t>>();

  uint32_t rcvd{0};
  auto allocators = createAllocators<uint32_t>(
      {start, start + kNumClients - 1},
      initVals,
      [&](int clientId, folly::Optional<uint32_t> newVal) {
        ASSERT(newVal);
        CHECK_EQ(clientId + start, newVal.value());
        rcvd++;
        if (rcvd == kNumClients) {
          LOG(INFO) << "We got everything, stopping eventLoop.";
          eventLoop.stop();
        }
      });

  eventLoop.run();
}

/**
 * Run all allocators with same seed value. Let them fight with each and finally
 * verify allocation. Each one should have some random value.
 */
TEST_P(RangeAllocatorFixture, NoSeed) {
  const uint64_t start = 61;
  const uint64_t end = start + kNumClients * 10;

  std::map<int /* client id */, uint64_t /* allocated value */> allocation;
  auto allocators = createAllocators<uint64_t>(
      {start, end},
      folly::none,
      [&](int clientId, folly::Optional<uint64_t> newVal) {
        if (newVal) {
          VLOG(1) << "client " << clientId << " got " << newVal.value();
          ASSERT_GE(newVal.value(), start);
          ASSERT_LE(newVal.value(), end);
          allocation[clientId] = newVal.value();
        } else {
          VLOG(1) << "client " << clientId << " lost its previous value";
          allocation.erase(clientId);
        }

        // Do validation and terminate loop if we got everything per our
        // expectations. Termination condition
        // - We must receive kNumClients unique values (one per client)
        if (allocation.size() != kNumClients) {
          return;
        }

        const auto allocatedVals = from(allocation) |
            map([](std::pair<int, uint64_t> const& kv) { return kv.second; }) |
            as<std::set<uint64_t>>();
        if (allocatedVals.size() == kNumClients) {
          LOG(INFO) << "We got everything, stopping eventLoop.";
          eventLoop.stop();
        }
      });

  eventLoop.run();

  for (size_t i = 0; i < allocators.size(); ++i) {
    EXPECT_FALSE(allocators[i]->isRangeConsumed());
    const auto maybeVal = allocators[i]->getValueFromKvStore();
    ASSERT_TRUE(maybeVal.hasValue());
    ASSERT_NE(allocation.end(), allocation.find(i));
    EXPECT_EQ(allocation[i], *maybeVal);
  }

  VLOG(2) << "=============== Allocation Table ===============";
  for (auto const& kv : allocation) {
    VLOG(2) << kv.first << "\t-->\t" << kv.second;
  }
}

/**
 * Run allocators with no seed but the range doesn't have enough allocation
 * space. In this case allocators with higher IDs will succeed and other
 * allocators will strive forever for the value.
 */
TEST_P(RangeAllocatorFixture, InsufficentRange) {
  // total of `kNumClients - 1` values available
  const uint32_t rangeSize = kNumClients - 1;
  const uint64_t start = 61;
  const uint64_t end = start + rangeSize - 1; // Range is inclusive

  std::map<int /* client id */, uint64_t /* allocated value */> allocation;
  auto allocators = createAllocators<uint64_t>(
      {start, end},
      folly::none,
      [&](int clientId, folly::Optional<uint64_t> newVal) {
        if (newVal) {
          const auto val = newVal.value();
          VLOG(1) << "client " << clientId << " got " << val;
          ASSERT_GE(val, start);
          ASSERT_LE(val, end);
          if (allocation.find(clientId) != allocation.end()) {
            // each newVal is only supposed to be called back once for a client
            EXPECT_NE(allocation.at(clientId), val);
          }
          allocation[clientId] = val;
        } else {
          VLOG(1) << "client " << clientId
                  << " lost it's previous value: " << allocation[clientId];
          allocation.erase(clientId);
        }

        // Do validate and terminate loop if we got everything as per our
        // expectations. Termination condition
        if (allocation.size() != rangeSize) {
          return;
        }
        const auto allocatedVals = from(allocation) |
            map([](std::pair<int, uint64_t> const& kv) { return kv.second; }) |
            as<std::set<uint64_t>>();
        if (allocatedVals.size() != rangeSize) {
          return;
        }
        // - All received values are unique
        if (not overrideOwner) {
          // no override
          LOG(INFO) << "We got everything, stopping eventLoop.";
          eventLoop.stop();
          return;
        }
        // Overide mode: client [1, rangeSize] must have received values
        // except 0-th client
        const auto expectedClientIds =
            range(1U, rangeSize + 1) | as<std::set<int>>();
        const auto clientIds = from(allocation) |
            map([](std::pair<int, uint64_t> const& kv) { return kv.first; }) |
            as<std::set<int>>();

        if (clientIds == expectedClientIds) {
          LOG(INFO) << "We got everything, stopping eventLoop.";
          eventLoop.stop();
        }
      });

  eventLoop.run();

  EXPECT_TRUE(allocators.front()->isRangeConsumed());

  VLOG(2) << "=============== Allocation Table ===============";
  for (auto const& kv : allocation) {
    VLOG(2) << kv.first << "\t-->\t" << kv.second;
  }
}

} // namespace openr

int
main(int argc, char** argv) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return -1;
  }

  return RUN_ALL_TESTS();
}
