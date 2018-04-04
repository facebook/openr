/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <utility>

#include <fbzmq/zmq/Zmq.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/config-store/PersistentStore.h>
#include <openr/config-store/PersistentStoreClient.h>

namespace openr {

TEST(PersistentStoreTest, LoadStoreEraseTest) {
  fbzmq::Context context;

  auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());
  const std::string filePath{
      folly::sformat("/tmp/aq_persistent_store_test_{}", tid)};
  const PersistentStoreUrl sockUrl1{"inproc://aq_persistent_store_test1"};
  const PersistentStoreUrl sockUrl2{"inproc://aq_persistent_store_test2"};

  // Data types to store/load
  const std::pair<std::string, uint32_t> keyVal1{"key1", 1235};
  const std::pair<std::string, std::string> keyVal2{"key2", "8375"};
  const std::pair<std::string, thrift::StoreDatabase> keyVal3{
      "key3",
      thrift::StoreDatabase(
          apache::thrift::FRAGILE,
          {
              {"fb", "facebook"},
              {"ig", "instagram"},
          })};

  std::unique_ptr<PersistentStoreClient> client;
  std::unique_ptr<PersistentStore> store;
  std::unique_ptr<std::thread> storeThread;

  //
  // Create new store and perform some operations on it
  //

  store = std::make_unique<PersistentStore>(filePath, sockUrl1, context);
  storeThread = std::make_unique<std::thread>([&]() { store->run(); });
  store->waitUntilRunning();
  client = std::make_unique<PersistentStoreClient>(sockUrl1, context);

  // Store and verify Load
  EXPECT_TRUE(client->store(keyVal1.first, keyVal1.second).value());
  EXPECT_TRUE(client->store(keyVal2.first, keyVal2.second).value());
  EXPECT_TRUE(client->storeThriftObj(keyVal3.first, keyVal3.second).value());
  EXPECT_EQ(keyVal1.second, client->load<uint32_t>(keyVal1.first).value());
  EXPECT_EQ(keyVal2.second, client->load<std::string>(keyVal2.first).value());
  EXPECT_EQ(
      keyVal3.second,
      client->loadThriftObj<thrift::StoreDatabase>(keyVal3.first).value());

  // Try overriding stuff
  const uint32_t val1 = 5321;
  EXPECT_NE(val1, keyVal1.second);
  EXPECT_TRUE(client->store(keyVal1.first, val1).value());
  EXPECT_EQ(val1, client->load<uint32_t>(keyVal1.first).value());
  EXPECT_NE(keyVal1.second, client->load<uint32_t>(keyVal1.first).value());

  // Try some wrong stuff
  EXPECT_TRUE(
      client->loadThriftObj<thrift::StoreDatabase>("unknown_key1").hasError());
  EXPECT_TRUE(client->load<uint32_t>("unknown_key2").hasError());

  // Try erase API. Subsequent erase of same key returns false
  EXPECT_TRUE(client->erase(keyVal3.first).value());
  EXPECT_FALSE(client->erase(keyVal3.first).value());

  //
  // Destroy store and re-create it, enforcing dump to file followed by
  // reloading of content on creation. Verify stored content is recovered.
  //

  store->stop();
  storeThread->join();
  storeThread.reset();
  store.reset();

  store = std::make_unique<PersistentStore>(filePath, sockUrl2, context);
  storeThread = std::make_unique<std::thread>([&]() { store->run(); });
  store->waitUntilRunning();
  client = std::make_unique<PersistentStoreClient>(sockUrl2, context);

  // Verify key1 and key2
  EXPECT_EQ(val1, client->load<uint32_t>(keyVal1.first).value());
  EXPECT_EQ(keyVal2.second, client->load<std::string>(keyVal2.first).value());

  // Stop store before exiting
  store->stop();
  storeThread->join();
  storeThread.reset();
  store.reset();
}

} // namespace openr

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
