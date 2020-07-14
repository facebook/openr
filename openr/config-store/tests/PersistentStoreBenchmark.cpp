/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Benchmark.h>
#include <folly/Random.h>
#include <folly/init/Init.h>
#include <openr/config-store/PersistentStoreWrapper.h>

namespace {
// kIterations <= n: change this to 10 singce n starts from 10,
// n is in BENCHMARK_PARAM(BM_PersistentStoreWrite, n)
uint32_t kIterations = 10;
} // namespace

namespace openr {

/**
 * Construct a vector with random strings inside
 */
std::vector<std::string>
constructRandomVector(uint32_t entryInStore) {
  std::vector<std::string> stringKeys;
  stringKeys.reserve(entryInStore);
  for (uint32_t i = 0; i < entryInStore; ++i) {
    stringKeys.push_back(folly::sformat("key-{}", i));
  }
  return stringKeys;
}

/**
 * Write keys with random values to store
 */
static void
writeKeyValueToStore(
    const std::vector<std::string>& stringKeys,
    PersistentStoreWrapper& store,
    const uint32_t skipStep) {
  for (size_t index = 0; index < stringKeys.size(); index += skipStep) {
    store
        ->store(
            stringKeys[index],
            folly::sformat("val-{}", folly::Random::rand32()))
        .get();
  }
}

/**
 * Erase all keys in store
 */
static void
eraseKeyFromStore(
    const std::vector<std::string>& stringKeys, PersistentStoreWrapper& store) {
  for (auto stringKey : stringKeys) {
    store->erase(stringKey).get();
  }
}

/**
 * Benchmark for writing keys to store
 * 1. Generate random keys
 * 2. Write keys to store
 * 3. Erase keys
 */
void
BM_PersistentStoreWrite(uint32_t iters, size_t numOfStringKeys) {
  auto suspender = folly::BenchmarkSuspender();
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

  // Create new storeWrapper and perform some operations on it
  auto store = std::make_unique<PersistentStoreWrapper>(tid);
  store->run();

  // Generate keys
  auto stringKeys = constructRandomVector(numOfStringKeys);
  writeKeyValueToStore(stringKeys, *store, 1);
  auto iterations =
      (stringKeys.size() / kIterations == 0) ? stringKeys.size() : kIterations;
  suspender.dismiss(); // Start measuring benchmark time

  // Write (key, random_value) pairs to store
  for (uint32_t i = 0; i < iters; i++) {
    writeKeyValueToStore(stringKeys, *store, stringKeys.size() / iterations);
  }

  suspender.rehire(); // Stop measuring time again
  // Erase the keys and stop store before exiting
  eraseKeyFromStore(stringKeys, *store);
}

/**
 * Benchmark for loading keys from store
 * 1. Generate random keys
 * 2. Write keys to store
 * 3. Load keys from store
 * 4. Erase keys
 */
void
BM_PersistentStoreLoad(uint32_t iters, size_t numOfStringKeys) {
  auto suspender = folly::BenchmarkSuspender();
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());
  // Create new storeWrapper and perform some operations on it
  auto store = std::make_unique<PersistentStoreWrapper>(tid);
  store->run();

  // Generate keys
  auto stringKeys = constructRandomVector(numOfStringKeys);
  writeKeyValueToStore(stringKeys, *store, 1);
  auto iterations =
      (stringKeys.size() / kIterations == 0) ? stringKeys.size() : kIterations;

  suspender.dismiss(); // Start measuring benchmark time
  for (uint32_t i = 0; i < iters; i++) {
    // Load value by key from store
    for (size_t index = 0; index < stringKeys.size();
         index += stringKeys.size() / iterations) {
      (*store)->load(stringKeys[index]);
    }
  }
  suspender.rehire(); // Stop measuring time again
  // Erase the keys and stop store before exiting
  eraseKeyFromStore(stringKeys, *store);
}

/**
 * Benchmark for Creating/Destroing a store
 * 1. Generate random keys
 * 2. Write keys to store
 * 3. Create and destroy a store
 */
void
BM_PersistentStoreCreateDestroy(uint32_t iters, size_t numOfStringKeys) {
  auto suspender = folly::BenchmarkSuspender();
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

  // Create storeWrapper and perform some operations on it
  auto store = std::make_unique<PersistentStoreWrapper>(tid + 1);
  store->run();

  // Generate keys
  auto stringKeys = constructRandomVector(numOfStringKeys);
  writeKeyValueToStore(stringKeys, *store, 1);

  // Reset original store
  store.reset();

  // Start measuring benchmark time
  suspender.dismiss();

  for (uint32_t i = 0; i < iters; i++) {
    // Create & destroy the store - ensure the same tid
    auto store1 = std::make_unique<PersistentStoreWrapper>(tid + 1);
  }
}

// The parameter is the number of keys already written to store
// before benchmarking the time.
BENCHMARK_PARAM(BM_PersistentStoreWrite, 10);
BENCHMARK_PARAM(BM_PersistentStoreWrite, 100);
BENCHMARK_PARAM(BM_PersistentStoreWrite, 1000);
BENCHMARK_PARAM(BM_PersistentStoreWrite, 10000);

BENCHMARK_PARAM(BM_PersistentStoreLoad, 10);
BENCHMARK_PARAM(BM_PersistentStoreLoad, 100);
BENCHMARK_PARAM(BM_PersistentStoreLoad, 1000);
BENCHMARK_PARAM(BM_PersistentStoreLoad, 10000);

BENCHMARK_PARAM(BM_PersistentStoreCreateDestroy, 10);
BENCHMARK_PARAM(BM_PersistentStoreCreateDestroy, 100);
BENCHMARK_PARAM(BM_PersistentStoreCreateDestroy, 1000);
BENCHMARK_PARAM(BM_PersistentStoreCreateDestroy, 10000);

} // namespace openr

int
main(int argc, char** argv) {
  folly::init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
