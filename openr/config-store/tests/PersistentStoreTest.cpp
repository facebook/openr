/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>
#include <utility>

#include <folly/FileUtil.h>
#include <folly/Random.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/config-store/PersistentStore.h>
#include <openr/config-store/PersistentStoreWrapper.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

using StoreDatabase = std::unordered_map<std::string, std::string>;

// Load database from disk
StoreDatabase
loadDatabaseFromDisk(const std::string& filePath) {
  StoreDatabase newDatabase;
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
    auto optionalObject = PersistentStore::decodePersistentObject(cursor);
    if (optionalObject.hasError()) {
      LOG(ERROR) << optionalObject.error();
    }

    // Read finish
    if (not optionalObject->has_value()) {
      break;
    }
    auto pObject = std::move(optionalObject->value());

    // Add/Delete persistentObject to/from 'database'
    if (pObject.type == ActionType::ADD) {
      newDatabase[pObject.key] =
          pObject.data.has_value() ? pObject.data.value() : "";
    } else if (pObject.type == ActionType::DEL) {
      newDatabase.erase(pObject.key);
    }
  }
  return newDatabase;
}

TEST(PersistentStoreTest, LoadStoreEraseTest) {
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

  thrift::PrefixMetrics metrics;
  metrics.path_preference_ref() = 100;
  metrics.source_preference_ref() = 200;

  // Data types to store/load
  const std::pair<std::string, std::string> keyVal2{"key2", "8375"};
  const std::pair<std::string, thrift::PrefixMetrics> keyVal3{"key3", metrics};
  const std::string val2 = "5321";

  {
    PersistentStoreWrapper store(tid);
    store.run();

    // Store and verify Load
    auto responseStoreKey2 = store->store(keyVal2.first, keyVal2.second).get();
    EXPECT_EQ(folly::Unit(), responseStoreKey2);

    auto responseStoreKey3 =
        store->storeThriftObj(keyVal3.first, keyVal3.second).get();
    EXPECT_EQ(folly::Unit(), responseStoreKey3);

    auto responseLoadKey2 = store->load(keyVal2.first).get();
    EXPECT_TRUE(responseLoadKey2);
    EXPECT_EQ(keyVal2.second, *responseLoadKey2);

    auto responseLoadKey3 =
        store->loadThriftObj<thrift::PrefixMetrics>(keyVal3.first).get();
    EXPECT_TRUE(responseLoadKey3.hasValue());
    EXPECT_EQ(keyVal3.second, responseLoadKey3.value());

    // Try overriding stuff
    EXPECT_NE(val2, keyVal2.second);
    auto responseOverrideKey2 = store->store(keyVal2.first, val2).get();
    EXPECT_EQ(folly::Unit(), responseOverrideKey2);

    auto responseReloadKey2 = store->load(keyVal2.first).get();
    EXPECT_TRUE(responseReloadKey2);
    EXPECT_EQ(val2, *responseReloadKey2);
    EXPECT_NE(keyVal2.second, *responseReloadKey2);

    // Try some wrong stuff
    EXPECT_TRUE(store->loadThriftObj<thrift::PrefixMetrics>("unknown_key1")
                    .get()
                    .hasError());
    EXPECT_FALSE(store->load("unknown_key2").get());

    // Try erase API. Subsequent erase of same key returns false
    auto responseEraseKey3 = store->erase(keyVal3.first).get();
    EXPECT_TRUE(responseEraseKey3);

    auto responseReEraseKey3 = store->erase(keyVal3.first).get();
    EXPECT_FALSE(responseReEraseKey3);
  }

  //
  // Destroy store and re-create it, enforcing dump to file followed by
  // reloading of content on creation. Verify stored content is recovered.
  //
  {
    PersistentStoreWrapper store(tid);
    store.run();

    // Verify key2

    auto responseReReLoadKey2 = store->load(keyVal2.first).get();
    EXPECT_TRUE(responseReReLoadKey2);
    EXPECT_EQ(val2, *responseReReLoadKey2);

    // Erase key before we end the test
    store->erase(keyVal2.first).get();
  }
}

TEST(PersistentStoreTest, EncodeDecodePersistentObject) {
  //
  // Verify the encodePersistentObject and decodePersistentObject
  //

  // Encode (ADD, key1, val1), and (DEL, key1),
  auto keyVal = std::make_pair("key1", "val1");
  PersistentObject pObjectAdd;
  pObjectAdd.type = ActionType::ADD;
  pObjectAdd.key = keyVal.first;
  pObjectAdd.data = keyVal.second;
  auto bufAdd = PersistentStore::encodePersistentObject(pObjectAdd);
  EXPECT_FALSE(bufAdd.hasError());

  PersistentObject pObjectDel;
  pObjectDel.type = ActionType::DEL;
  pObjectDel.key = keyVal.first;
  auto bufDel = PersistentStore::encodePersistentObject(pObjectDel);
  EXPECT_FALSE(bufDel.hasError());

  auto buf = folly::IOBuf::create((*bufAdd)->length() + (*bufDel)->length());
  folly::io::Appender appender(buf.get(), 0);
  appender.push((*bufAdd)->data(), (*bufAdd)->length());
  appender.push((*bufDel)->data(), (*bufDel)->length());

  // Decode (ADD, key1, val1), and (DEL, key1)
  folly::io::Cursor cursor(buf.get());
  auto optionalObject = PersistentStore::decodePersistentObject(cursor);
  EXPECT_FALSE(optionalObject.hasError());
  EXPECT_TRUE(optionalObject->has_value());

  auto pObjectGetAdd = optionalObject->value();
  EXPECT_EQ(keyVal.first, pObjectGetAdd.key);
  EXPECT_EQ(keyVal.second, pObjectGetAdd.data.value());

  optionalObject = PersistentStore::decodePersistentObject(cursor);
  EXPECT_FALSE(optionalObject.hasError());
  EXPECT_TRUE(optionalObject->has_value());

  auto pObjectGetDel = optionalObject->value();
  EXPECT_EQ(keyVal.first, pObjectGetDel.key);
  EXPECT_EQ(false, pObjectGetDel.data.has_value());
}

TEST(PersistentStoreTest, BulkStoreLoad) {
  const auto tid = std::hash<std::thread::id>()(std::this_thread::get_id());

  StoreDatabase database;
  std::string filePath;
  {
    PersistentStoreWrapper store(tid);
    store.run();
    filePath = store.filePath;

    //
    // Vefirying whether 100 pobjects can be successfully written to disk
    //

    for (auto index = 0; index < 100; index++) {
      const std::pair<std::string, std::string> tmpKeyVal{
          fmt::format("key-{}", index),
          fmt::format("val-{}", folly::Random::rand32())};

      database[tmpKeyVal.first] = tmpKeyVal.second;
      auto responseStoreTmpKey =
          store->store(tmpKeyVal.first, tmpKeyVal.second).get();
      EXPECT_EQ(folly::Unit(), responseStoreTmpKey);
    }

    // Stop & destroy store before exiting
  }

  {
    // Load file from disk
    auto databaseStore = loadDatabaseFromDisk(filePath);
    EXPECT_EQ(database, databaseStore);
  }
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
