/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include <folly/init/Init.h>
#include <gtest/gtest.h>
#include <sodium.h>

#include <openr/allocators/RangeAllocator.h>
#include <openr/config/Config.h>
#include <openr/config/tests/Utils.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace folly::gen;

namespace openr {

namespace {

// the size of the value string
const uint32_t kValueStrSize = 64;

// interval for periodic syncs
const std::chrono::seconds kDbSyncInterval(1);

// Count for testing purpose
const uint32_t kNumStores = 3; // Total number of KvStore
const uint32_t kNumClients = 99; // Total number of KvStoreClientInternal
} // namespace

/**
 * Base class for all of our unit-tests. Internally it has linear topology of
 * kNumStores KvStores, total of kNumClients KvStoreClientInternals evenly
 * distributed among these KvStores.
 *
 * All of our tests create kNumClients allocators (one attached with each
 * KvStoreClientInternal).
 */
class RangeAllocatorFixture : public ::testing::TestWithParam<bool> {
 public:
  /**
   * Create linear topology of KvStores for data flow. All of our unittests
   * for range allocations are based on this linear topology.
   */
  void
  SetUp() override {
    for (uint32_t i = 0; i < kNumStores; i++) {
      auto config = std::make_shared<Config>(
          getBasicOpenrConfig(folly::sformat("store{}", i + 1)));
      auto store = std::make_unique<KvStoreWrapper>(config);
      stores.emplace_back(std::move(store));
      configs.emplace_back(std::move(config));
      stores.back()->run();
    }

    for (uint32_t i = 1; i < kNumStores; i++) {
      stores[i - 1]->addPeer(
          kTestingAreaName, stores[i]->getNodeId(), stores[i]->getPeerSpec());
      stores[i]->addPeer(
          kTestingAreaName,
          stores[i - 1]->getNodeId(),
          stores[i - 1]->getPeerSpec());
    }

    for (const auto& store : stores) {
      store->recvKvStoreSyncedSignal();
    }

    for (uint32_t i = 0; i < kNumClients; i++) {
      auto const& store = stores[i % kNumStores];
      auto client = std::make_unique<KvStoreClientInternal>(
          &evb, createClientName(i), store->getKvStore());
      clients.emplace_back(std::move(client));
    }
  }

  void
  TearDown() override {
    for (auto& store : stores) {
      store->stop();
    }
    for (auto& client : clients) {
      client.reset();
    }
    clients.clear();
    stores.clear();

    evb.stop();
    evb.waitUntilStopped();
    evbThread.join();
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
      const std::optional<std::vector<T>> maybeInitVals,
      std::function<void(int /* client id */, std::optional<T>)> callback,
      const std::chrono::milliseconds rangeAllocTtl =
          Constants::kRangeAllocTtl) {
    // sanity check
    if (maybeInitVals) {
      CHECK_EQ(clients.size(), maybeInitVals->size());
    }

    using namespace std::chrono_literals;
    std::vector<std::unique_ptr<RangeAllocator<T>>> allocators;
    for (size_t i = 0; i < clients.size(); i++) {
      auto allocator = std::make_unique<RangeAllocator<T>>(
          kTestingAreaName,
          createClientName(i),
          "value:",
          clients[i].get(),
          [callback, i](std::optional<T> newVal) noexcept {
            callback(i, newVal);
          },
          10ms /* min backoff */,
          100ms /* max backoff */,
          overrideOwner /* override allowed */,
          nullptr,
          rangeAllocTtl);
      // start allocator
      allocator->startAllocator(
          allocRange,
          maybeInitVals ? std::make_optional(maybeInitVals->at(i))
                        : std::nullopt);
      allocators.emplace_back(std::move(allocator));
    }

    return std::move(allocators);
  }

  // Linear topology of stores. i <--> i+1 <--> i+2 ......
  std::vector<std::unique_ptr<KvStoreWrapper>> stores;
  std::vector<std::shared_ptr<Config>> configs;

  // Client `i` connects to store `i % stores.size()`
  // All clients loop in same event-loop
  std::vector<std::unique_ptr<KvStoreClientInternal>> clients;

  // OpenrEventBase for all clients
  OpenrEventBase evb;
  std::thread evbThread;

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

  folly::Baton waitBaton;
  uint32_t rcvd{0};
  auto allocators = createAllocators<uint32_t>(
      {start, start + kNumClients - 1},
      initVals,
      [&](int clientId, std::optional<uint32_t> newVal) {
        DCHECK(newVal);
        CHECK_EQ(clientId + start, newVal.value());
        rcvd++;
        if (rcvd == kNumClients) {
          LOG(INFO) << "We got everything, stopping OpenrEventBase.";

          // Synchronization primitive
          waitBaton.post();
        }
      });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  for (auto& allocator : allocators) {
    allocator.reset();
  }
}

/**
 * Run all allocators with same seed value. Let them fight with each and finally
 * verify allocation. Each one should have some random value.
 */
TEST_P(RangeAllocatorFixture, NoSeed) {
  const uint64_t start = 61;
  const uint64_t end = start + kNumClients * 10;

  folly::Baton waitBaton;
  std::map<int /* client id */, uint64_t /* allocated value */> allocation;
  auto allocators = createAllocators<uint64_t>(
      {start, end},
      std::nullopt,
      [&](int clientId, std::optional<uint64_t> newVal) {
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
          LOG(INFO) << "We got everything, stopping OpenrEventBase.";

          // Synchronization primitive
          waitBaton.post();
        }
      });

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  for (size_t i = 0; i < allocators.size(); ++i) {
    evb.getEvb()->runInEventBaseThreadAndWait([&]() {
      EXPECT_FALSE(allocators[i]->isRangeConsumed());
      const auto maybeVal = allocators[i]->getValueFromKvStore();
      ASSERT_TRUE(maybeVal.has_value());
      ASSERT_NE(allocation.end(), allocation.find(i));
      EXPECT_EQ(allocation[i], *maybeVal);
    });
  }

  VLOG(2) << "=============== Allocation Table ===============";
  for (auto const& kv : allocation) {
    VLOG(2) << kv.first << "\t-->\t" << kv.second;
  }

  for (auto& allocator : allocators) {
    allocator.reset();
  }
}

/**
 * Run allocators with no seed but the range doesn't have enough allocation
 * space. In this case allocators with higher IDs will succeed and other
 * allocators will strive forever for the value. Ensure other allocators are
 * continuously retrying, by removing an allocator and making sure another
 * client picks up newly freed value.
 */
TEST_P(RangeAllocatorFixture, InsufficentRange) {
  // total of `kNumClients - 1` values available
  uint32_t expectedClientIdStart = 1U;
  uint32_t expectedClientIdEnd = kNumClients;
  const uint32_t rangeSize = kNumClients - 1;
  const uint64_t start = 61;
  const uint64_t end = start + rangeSize - 1; // Range is inclusive

  folly::Baton waitBaton;

  // Limit only one post() call for the wait().
  // The UT is possible to call baton.post() more than one time,
  // which will cause a fatal error.
  bool isPost = false;
  auto batonPostOnce = [&]() {
    if (!isPost) {
      isPost = true;
      waitBaton.post();
    }
  };
  std::map<int /* client id */, uint64_t /* allocated value */> allocation;
  auto allocators = createAllocators<uint64_t>(
      {start, end},
      std::nullopt,
      [&](int clientId, std::optional<uint64_t> newVal) {
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
          LOG(INFO) << "We got everything, stopping OpenrEventBase.";
          // Synchronization primitive
          batonPostOnce();
          return;
        }
        // Overide mode: client [expectedClientIdStart, expectedClientIdEnd]
        // must have received values. The first run should be [1, kNumClients].
        // The second run should be [0, kNumClients - 1].
        const auto expectedClientIds =
            range(expectedClientIdStart, expectedClientIdEnd) |
            as<std::set<int>>();
        const auto clientIds = from(allocation) |
            map([](std::pair<int, uint64_t> const& kv) { return kv.first; }) |
            as<std::set<int>>();

        if (clientIds == expectedClientIds) {
          LOG(INFO) << "We got everything, stopping OpenrEventBase.";
          // Synchronization primitive
          batonPostOnce();
        }
      },
      4s);

  // Start the event loop and wait until it is finished execution.
  evbThread = std::thread([&]() { evb.run(); });
  evb.waitUntilRunning();

  // Synchronization primitive
  waitBaton.wait();

  while (true) {
    // Wait for kvstore updated.
    // Because in the rare case that isRangeConsumed() could dump and check all
    // keys earlier than all the keys are actually set into the kvstore.
    bool isRangeConsumed{false};
    evb.getEvb()->runInEventBaseThreadAndWait(
        [&]() { isRangeConsumed = allocators.front()->isRangeConsumed(); });
    if (isRangeConsumed) {
      break;
    }
    std::this_thread::yield();
  }

  VLOG(2) << "=============== Allocation Table ===============";
  for (auto const& kv : allocation) {
    VLOG(2) << kv.first << "\t-->\t" << kv.second;
  }

  // Schedule timeout to remove an allocator (client). This should then free a
  // prefix for the last, unallocated, allocator.
  if (not overrideOwner) {
    for (uint32_t i = 0; i < kNumClients; i++) {
      if (allocators[i]->getValue().has_value()) {
        VLOG(1) << "Removing client " << i << " with value "
                << allocators[i]->getValue().value();
        allocation.erase(i);
        allocators[i].reset();
        allocators.erase(allocators.begin() + i);
        break;
      }
    }
  } else {
    expectedClientIdStart--;
    expectedClientIdEnd--;
    allocation.erase(kNumClients - 1);
    allocators.pop_back();
  }

  evb.getEvb()->runInEventBaseThreadAndWait(
      [&]() { EXPECT_TRUE(allocators.front()->isRangeConsumed()); });

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
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return -1;
  }

  return RUN_ALL_TESTS();
}
