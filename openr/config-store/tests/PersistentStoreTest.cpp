/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <utility>

#include <fbzmq/zmq/Zmq.h>
#include <folly/FileUtil.h>
#include <folly/Random.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/config-store/PersistentStore.h>
#include <openr/config-store/PersistentStoreClient.h>

namespace openr {

// Load database from disk
thrift::StoreDatabase
loadDatabaseFromDisk(
    const std::string& filePath, std::unique_ptr<PersistentStore>& store) {
  thrift::StoreDatabase newDatabase;
  std::string fileData("");
  if (not folly::readFile(filePath.c_str(), fileData)) {
    return newDatabase;
  }

  auto ioBuf = folly::IOBuf::wrapBuffer(fileData.c_str(), fileData.size());
  folly::io::Cursor cursor(ioBuf.get());

  // Read 'kTlvFormatMarker'
  cursor.readFixedString(kTlvFormatMarker.size());
  // Iteratively read persistentObject from disk
  while (true) {
    auto optionalObject = store->decodePersistentObject(cursor);
    if (optionalObject.hasError()) {
      LOG(ERROR) << optionalObject.error();
    }

    // Read finish
    if (not optionalObject->hasValue()) {
      break;
    }
    auto pObject = std::move(optionalObject->value());

    // Add/Delete persistentObject to/from 'database'
    if (pObject.type == ActionType::ADD) {
      newDatabase.keyVals[pObject.key] =
          pObject.data.has_value() ? pObject.data.value() : "";
    } else if (pObject.type == ActionType::DEL) {
      newDatabase.keyVals.erase(pObject.key);
    }
  }
  return newDatabase;
}

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

  const std::string inprocSocket{folly::sformat("1-{}", tid)};
  store = std::make_unique<PersistentStore>(
      inprocSocket, filePath, sockUrl1, context);
  storeThread = std::make_unique<std::thread>([&]() { store->run(); });
  store->waitUntilRunning();
  client = std::make_unique<PersistentStoreClient>(sockUrl1, context);

  // Store and verify Load
  auto responseStoreKey1 = client->store(keyVal1.first, keyVal1.second);
  EXPECT_TRUE(responseStoreKey1.hasValue());
  EXPECT_TRUE(responseStoreKey1.value());

  auto responseStoreKey2 = client->store(keyVal2.first, keyVal2.second);
  EXPECT_TRUE(responseStoreKey2.hasValue());
  EXPECT_TRUE(responseStoreKey2.value());

  auto responseStoreKey3 =
      client->storeThriftObj(keyVal3.first, keyVal3.second);
  EXPECT_TRUE(responseStoreKey3.hasValue());
  EXPECT_TRUE(responseStoreKey3.value());

  auto responseLoadKey1 = client->load<uint32_t>(keyVal1.first);
  EXPECT_TRUE(responseLoadKey1.hasValue());
  EXPECT_EQ(keyVal1.second, responseLoadKey1.value());

  auto responseLoadKey2 = client->load<std::string>(keyVal2.first);
  EXPECT_TRUE(responseLoadKey2.hasValue());
  EXPECT_EQ(keyVal2.second, responseLoadKey2.value());

  auto responseLoadKey3 =
      client->loadThriftObj<thrift::StoreDatabase>(keyVal3.first);
  EXPECT_TRUE(responseLoadKey3.hasValue());
  EXPECT_EQ(keyVal3.second, responseLoadKey3.value());

  // Try overriding stuff
  const uint32_t val1 = 5321;
  EXPECT_NE(val1, keyVal1.second);
  auto responseOverrideKey1 = client->store(keyVal1.first, val1);
  EXPECT_TRUE(responseOverrideKey1.hasValue());
  EXPECT_TRUE(responseOverrideKey1.value());

  auto responseReloadKey1 = client->load<uint32_t>(keyVal1.first);
  EXPECT_TRUE(responseReloadKey1.hasValue());
  EXPECT_EQ(val1, responseReloadKey1.value());
  EXPECT_NE(keyVal1.second, responseReloadKey1.value());

  // Try some wrong stuff
  EXPECT_TRUE(
      client->loadThriftObj<thrift::StoreDatabase>("unknown_key1").hasError());
  EXPECT_TRUE(client->load<uint32_t>("unknown_key2").hasError());

  // Try erase API. Subsequent erase of same key returns false
  auto responseEraseKey3 = client->erase(keyVal3.first);
  EXPECT_TRUE(responseEraseKey3.hasValue());
  EXPECT_TRUE(responseEraseKey3.value());

  auto responseReEraseKey3 = client->erase(keyVal3.first);
  EXPECT_TRUE(responseReEraseKey3.hasValue());
  EXPECT_FALSE(responseReEraseKey3.value());

  //
  // Destroy store and re-create it, enforcing dump to file followed by
  // reloading of content on creation. Verify stored content is recovered.
  //

  store->stop();
  storeThread->join();
  storeThread.reset();
  store.reset();

  store = std::make_unique<PersistentStore>("1", filePath, sockUrl2, context);
  storeThread = std::make_unique<std::thread>([&]() { store->run(); });
  store->waitUntilRunning();
  client = std::make_unique<PersistentStoreClient>(sockUrl2, context);

  // Verify key1 and key2
  auto responseReReLoadKey1 = client->load<uint32_t>(keyVal1.first);
  EXPECT_TRUE(responseReReLoadKey1.hasValue());
  EXPECT_EQ(val1, responseReReLoadKey1.value());

  auto responseReLoadKey2 = client->load<std::string>(keyVal2.first);
  EXPECT_TRUE(responseReLoadKey2.hasValue());
  EXPECT_EQ(keyVal2.second, responseReLoadKey2.value());

  //
  // Verify the encodePersistentObject and decodePersistentObject
  //

  // Encode (ADD, key1, val1), and (DEL, key1),
  auto keyVal = std::make_pair("key1", "val1");
  PersistentObject pObjectAdd;
  pObjectAdd.type = ActionType::ADD;
  pObjectAdd.key = keyVal.first;
  pObjectAdd.data = keyVal.second;
  auto bufAdd = store->encodePersistentObject(pObjectAdd);
  EXPECT_FALSE(bufAdd.hasError());

  PersistentObject pObjectDel;
  pObjectDel.type = ActionType::DEL;
  pObjectDel.key = keyVal.first;
  auto bufDel = store->encodePersistentObject(pObjectDel);
  EXPECT_FALSE(bufDel.hasError());

  auto buf = folly::IOBuf::create((*bufAdd)->length() + (*bufDel)->length());
  folly::io::Appender appender(buf.get(), 0);
  appender.push((*bufAdd)->data(), (*bufAdd)->length());
  appender.push((*bufDel)->data(), (*bufDel)->length());

  // Decode (ADD, key1, val1), and (DEL, key1)
  folly::io::Cursor cursor(buf.get());
  auto optionalObject = store->decodePersistentObject(cursor);
  EXPECT_FALSE(optionalObject.hasError());
  EXPECT_TRUE(optionalObject->hasValue());

  auto pObjectGetAdd = optionalObject->value();
  EXPECT_EQ(keyVal.first, pObjectGetAdd.key);
  EXPECT_EQ(keyVal.second, pObjectGetAdd.data.value());

  optionalObject = store->decodePersistentObject(cursor);
  EXPECT_FALSE(optionalObject.hasError());
  EXPECT_TRUE(optionalObject->hasValue());

  auto pObjectGetDel = optionalObject->value();
  EXPECT_EQ(keyVal.first, pObjectGetDel.key);
  EXPECT_EQ(false, pObjectGetDel.data.has_value());

  //
  // Vefirying whether 100 pobjects can be successfully written to disk
  //

  thrift::StoreDatabase database;
  for (auto index = 0; index < 100; index++) {
    const std::pair<std::string, std::string> tmpKeyVal{
        folly::sformat("key-{}", index),
        folly::sformat("val-{}", folly::Random::rand32())};

    database.keyVals[tmpKeyVal.first] = tmpKeyVal.second;
    auto responseStoreTmpKey = client->store(tmpKeyVal.first, tmpKeyVal.second);
    EXPECT_TRUE(responseStoreTmpKey.hasValue());
    EXPECT_TRUE(responseStoreTmpKey.value());
  }

  // Stop store before exiting
  store->stop();
  storeThread->join();
  storeThread.reset();
  store.reset();

  // Load file from disk
  auto databaseStore = loadDatabaseFromDisk(filePath, store);
  // Delete previously added two keys
  databaseStore.keyVals.erase(keyVal1.first);
  databaseStore.keyVals.erase(keyVal2.first);
  EXPECT_EQ(database, databaseStore);
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
