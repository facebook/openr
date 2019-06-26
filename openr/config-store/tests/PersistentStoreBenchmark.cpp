/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>
#include <folly/Random.h>
#include <openr/config-store/PersistentStoreClient.h>
#include <openr/config-store/PersistentStoreWrapper.h>

namespace {
// kIterations
uint32_t kIterations = 1000;
} // namespace

namespace openr {

std::vector<std::string>
constructRandomVector(uint32_t entryInStore) {
  std::vector<std::string> stringKeys;
  stringKeys.reserve(entryInStore);
  for (auto i = 0; i < entryInStore; ++i) {
    stringKeys.push_back(folly::sformat("key-{}", i));
  }
  return stringKeys;
}

static void
writeKeyValueToStore(
    const std::vector<std::string>& stringKeys,
    const std::unique_ptr<PersistentStoreClient>& client,
    const uint32_t skipStep) {
  for (auto index = 0; index < stringKeys.size(); index += skipStep) {
    client->store(
        stringKeys[index], folly::sformat("val-{}", folly::Random::rand32()));
  }
}

static void
eraseKeyFromStore(
    const std::vector<std::string>& stringKeys,
    const std::unique_ptr<PersistentStoreClient>& client) {
  for (auto stringKey : stringKeys) {
    client->erase(stringKey);
  }
}

void
BM_PersistentStoreWrite(benchmark::State& state) {
  fbzmq::Context context;
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

  // Create new storeWrapper and perform some operations on it
  auto storeWrapper = std::make_unique<PersistentStoreWrapper>(context, tid);
  storeWrapper->run();
  auto client = std::make_unique<PersistentStoreClient>(
      PersistentStoreUrl{storeWrapper->sockUrl}, context);

  // Generate keys
  auto stringKeys = constructRandomVector(state.range(0));
  writeKeyValueToStore(stringKeys, client, 1);
  auto iterations =
      (stringKeys.size() / kIterations == 0) ? stringKeys.size() : kIterations;

  for (auto _ : state) {
    // Write (key, random_value) pairs to store
    writeKeyValueToStore(stringKeys, client, stringKeys.size() / iterations);
  }

  // Erase the keys and stop store before exiting
  eraseKeyFromStore(stringKeys, client);
  storeWrapper->stop();
}

void
BM_PersistentStoreLoad(benchmark::State& state) {
  fbzmq::Context context;
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());
  // Create new storeWrapper and perform some operations on it
  auto storeWrapper = std::make_unique<PersistentStoreWrapper>(context, tid);
  storeWrapper->run();
  auto client = std::make_unique<PersistentStoreClient>(
      PersistentStoreUrl{storeWrapper->sockUrl}, context);

  // Generate keys
  auto stringKeys = constructRandomVector(state.range(0));
  writeKeyValueToStore(stringKeys, client, 1);
  auto iterations =
      (stringKeys.size() / kIterations == 0) ? stringKeys.size() : kIterations;

  for (auto _ : state) {
    // Load value by key from store
    for (auto index = 0; index < stringKeys.size();
         index += stringKeys.size() / iterations) {
      client->load<std::string>(stringKeys[index]);
    }
  }

  // Erase the keys and stop store before exiting
  eraseKeyFromStore(stringKeys, client);
  storeWrapper->stop();
}

void
BM_PersistentStoreCreateDestroy(benchmark::State& state) {
  fbzmq::Context context;

  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

  for (auto _ : state) {
    // Create new storeWrapper and perform some operations on it
    auto storeWrapper = std::make_unique<PersistentStoreWrapper>(context, tid);
  }
}

} // namespace openr
