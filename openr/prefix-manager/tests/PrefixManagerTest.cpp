/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/prefix-manager/PrefixManagerClient.h>

using namespace openr;

using apache::thrift::CompactSerializer;

namespace {

const std::string kConfigStoreUrl = "inproc://pm_ut_config_store";
const std::string kPrefixManagerGlobalCmdUrl = "inproc://pm_ut_global_cmd_url";

const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");
const auto addr5 = toIpPrefix("ffff:10:1:5::/64");
const auto addr6 = toIpPrefix("ffff:10:2:6::/64");
const auto addr7 = toIpPrefix("ffff:10:3:7::/64");
const auto addr8 = toIpPrefix("ffff:10:4:8::/64");
const auto addr9 = toIpPrefix("ffff:10:4:9::/64");
const auto addr10 = toIpPrefix("ffff:10:4:10::/64");

const auto prefixEntry1 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr1,
    thrift::PrefixType::DEFAULT,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto prefixEntry2 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr2,
    thrift::PrefixType::PREFIX_ALLOCATOR,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto prefixEntry3 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr3,
    thrift::PrefixType::DEFAULT,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto prefixEntry4 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr4,
    thrift::PrefixType::PREFIX_ALLOCATOR,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto prefixEntry5 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr5,
    thrift::PrefixType::DEFAULT,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto prefixEntry6 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr6,
    thrift::PrefixType::PREFIX_ALLOCATOR,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto prefixEntry7 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr7,
    thrift::PrefixType::DEFAULT,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto prefixEntry8 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr8,
    thrift::PrefixType::PREFIX_ALLOCATOR,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto ephemeralPrefixEntry9 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr9,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    true);
const auto persistentPrefixEntry9 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr9,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    false);
const auto ephemeralPrefixEntry10 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr10,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    true);
const auto persistentPrefixEntry10 = thrift::PrefixEntry(
    apache::thrift::FRAGILE,
    addr10,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    false);
} // namespace

class PrefixManagerTestFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    // spin up a config store
    storageFilePath = folly::sformat(
        "/tmp/pm_ut_config_store.bin.{}",
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    configStore = std::make_unique<PersistentStore>(
        "1",
        storageFilePath,
        PersistentStoreUrl{kConfigStoreUrl},
        context,
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(0));

    configStoreThread = std::make_unique<std::thread>([this]() noexcept {
      LOG(INFO) << "ConfigStore thread starting";
      configStore->run();
      LOG(INFO) << "ConfigStore thread finishing";
    });
    configStore->waitUntilRunning();

    // spin up a kvstore
    kvStoreWrapper = std::make_shared<KvStoreWrapper>(
        context,
        "test_store1",
        std::chrono::seconds(1) /* db sync interval */,
        std::chrono::seconds(600) /* counter submit interval */,
        std::unordered_map<std::string, thrift::PeerSpec>{});
    kvStoreWrapper->run();
    LOG(INFO) << "The test KV store is running";

    kvStoreClient = std::make_unique<KvStoreClient>(
        context,
        &evl,
        "node-1",
        kvStoreWrapper->localCmdUrl,
        kvStoreWrapper->localPubUrl);
    // start a prefix manager
    prefixManager = std::make_unique<PrefixManager>(
        "node-1",
        std::string{kPrefixManagerGlobalCmdUrl},
        PersistentStoreUrl{kConfigStoreUrl},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        MonitorSubmitUrl{"inproc://monitor_submit"},
        PrefixDbMarker{"prefix:"},
        false /* prefix-mananger perf measurement */,
        std::chrono::seconds{0},
        Constants::kKvStoreDbTtl,
        context);

    prefixManagerThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "PrefixManager thread starting";
      prefixManager->run();
      LOG(INFO) << "PrefixManager thread finishing";
    });
    prefixManager->waitUntilRunning();

    prefixManagerClient = std::make_unique<PrefixManagerClient>(
        PrefixManagerLocalCmdUrl{prefixManager->inprocCmdUrl}, context);
  }

  void
  TearDown() override {
    // this will be invoked before linkMonitorThread's d-tor
    LOG(INFO) << "Stopping the linkMonitor thread";
    prefixManager->stop();
    prefixManagerThread->join();

    // Erase data from config store
    PersistentStoreClient configStoreClient{PersistentStoreUrl{kConfigStoreUrl},
                                            context};
    configStoreClient.erase("prefix-manager-config");

    // stop config store
    configStore->stop();
    configStoreThread->join();

    // stop the kvStore
    kvStoreWrapper->stop();
    LOG(INFO) << "The test KV store is stopped";
  }

  fbzmq::Context context;

  fbzmq::ZmqEventLoop evl;

  std::string storageFilePath;
  std::unique_ptr<PersistentStore> configStore;
  std::unique_ptr<std::thread> configStoreThread;

  // Create the serializer for write/read
  CompactSerializer serializer;
  std::unique_ptr<PrefixManager> prefixManager{nullptr};
  std::unique_ptr<PrefixManagerClient> prefixManagerClient{nullptr};
  std::unique_ptr<std::thread> prefixManagerThread{nullptr};
  std::shared_ptr<KvStoreWrapper> kvStoreWrapper{nullptr};
  std::unique_ptr<KvStoreClient> kvStoreClient{nullptr};
};

TEST_F(PrefixManagerTestFixture, AddRemovePrefix) {
  auto resp1 = prefixManagerClient->withdrawPrefixes({prefixEntry1});
  auto resp2 = prefixManagerClient->addPrefixes({prefixEntry1});
  auto resp3 = prefixManagerClient->addPrefixes({prefixEntry1});
  auto resp4 = prefixManagerClient->withdrawPrefixes({prefixEntry1});
  auto resp5 = prefixManagerClient->withdrawPrefixes({prefixEntry3});
  auto resp6 = prefixManagerClient->addPrefixes({prefixEntry2});
  auto resp7 = prefixManagerClient->addPrefixes({prefixEntry3});
  auto resp8 = prefixManagerClient->addPrefixes({prefixEntry4});
  auto resp9 = prefixManagerClient->addPrefixes({prefixEntry3});
  auto resp10 = prefixManagerClient->withdrawPrefixes({prefixEntry2});
  auto resp11 = prefixManagerClient->withdrawPrefixes({prefixEntry3});
  auto resp12 = prefixManagerClient->withdrawPrefixes({prefixEntry4});
  auto resp13 = prefixManagerClient->addPrefixes(
      {prefixEntry1, prefixEntry2, prefixEntry3});
  auto resp14 =
      prefixManagerClient->withdrawPrefixes({prefixEntry1, prefixEntry2});
  auto resp15 =
      prefixManagerClient->withdrawPrefixes({prefixEntry1, prefixEntry2});
  auto resp16 = prefixManagerClient->withdrawPrefixes({prefixEntry4});
  auto resp17 = prefixManagerClient->addPrefixes({ephemeralPrefixEntry9});
  auto resp18 = prefixManagerClient->withdrawPrefixes({ephemeralPrefixEntry9});
  EXPECT_FALSE(resp1.value().success);
  EXPECT_TRUE(resp2.value().success);
  EXPECT_FALSE(resp3.value().success);
  EXPECT_TRUE(resp4.value().success);
  EXPECT_FALSE(resp5.value().success);
  EXPECT_TRUE(resp6.value().success);
  EXPECT_TRUE(resp7.value().success);
  EXPECT_TRUE(resp8.value().success);
  EXPECT_FALSE(resp9.value().success);
  EXPECT_TRUE(resp10.value().success);
  EXPECT_TRUE(resp11.value().success);
  EXPECT_TRUE(resp12.value().success);
  EXPECT_TRUE(resp13.value().success);
  EXPECT_TRUE(resp14.value().success);
  EXPECT_FALSE(resp15.value().success);
  EXPECT_FALSE(resp16.value().success);
  EXPECT_TRUE(resp17.value().success);
  EXPECT_TRUE(resp18.value().success);
}

TEST_F(PrefixManagerTestFixture, RemoveUpdateType) {
  prefixManagerClient->addPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  prefixManagerClient->addPrefixes({prefixEntry3});
  prefixManagerClient->addPrefixes({prefixEntry4});
  prefixManagerClient->addPrefixes({prefixEntry5});
  prefixManagerClient->addPrefixes({prefixEntry6});
  prefixManagerClient->addPrefixes({prefixEntry7});
  prefixManagerClient->addPrefixes({prefixEntry8});
  auto resp1 = prefixManagerClient->withdrawPrefixes({prefixEntry1});
  EXPECT_TRUE(resp1.value().success);
  auto resp2 =
      prefixManagerClient->withdrawPrefixesByType(thrift::PrefixType::DEFAULT);
  EXPECT_TRUE(resp2.value().success);
  // can't withdraw twice
  auto resp3 =
      prefixManagerClient->withdrawPrefixesByType(thrift::PrefixType::DEFAULT);
  EXPECT_FALSE(resp3.value().success);
  // all the DEFAULT type should be gone
  auto resp4 = prefixManagerClient->withdrawPrefixes({prefixEntry3});
  EXPECT_FALSE(resp4.value().success);
  auto resp5 = prefixManagerClient->withdrawPrefixes({prefixEntry5});
  EXPECT_FALSE(resp5.value().success);
  auto resp6 = prefixManagerClient->withdrawPrefixes({prefixEntry7});
  EXPECT_FALSE(resp6.value().success);
  // The PREFIX_ALLOCATOR type should still be there to be withdrawed
  auto resp7 = prefixManagerClient->withdrawPrefixes({prefixEntry2});
  EXPECT_TRUE(resp7.value().success);
  auto resp8 = prefixManagerClient->withdrawPrefixes({prefixEntry4});
  EXPECT_TRUE(resp8.value().success);
  auto resp9 = prefixManagerClient->withdrawPrefixes({prefixEntry6});
  EXPECT_TRUE(resp9.value().success);
  auto resp10 = prefixManagerClient->withdrawPrefixes({prefixEntry8});
  EXPECT_TRUE(resp10.value().success);
  auto resp11 = prefixManagerClient->withdrawPrefixesByType(
      thrift::PrefixType::PREFIX_ALLOCATOR);
  EXPECT_FALSE(resp11.value().success);
  // update all allocated prefixes
  prefixManagerClient->addPrefixes({prefixEntry2, prefixEntry4});

  // Test sync logic
  auto resp12 = prefixManagerClient->syncPrefixesByType(
      thrift::PrefixType::PREFIX_ALLOCATOR, {prefixEntry6, prefixEntry8});
  auto resp13 = prefixManagerClient->syncPrefixesByType(
      thrift::PrefixType::PREFIX_ALLOCATOR, {prefixEntry6, prefixEntry8});
  EXPECT_TRUE(resp12.value().success);
  EXPECT_FALSE(resp13.value().success);

  EXPECT_FALSE(
      prefixManagerClient->withdrawPrefixes({prefixEntry2}).value().success);
  EXPECT_FALSE(
      prefixManagerClient->withdrawPrefixes({prefixEntry4}).value().success);
  EXPECT_TRUE(
      prefixManagerClient->withdrawPrefixes({prefixEntry6}).value().success);
  EXPECT_TRUE(
      prefixManagerClient->withdrawPrefixes({prefixEntry8}).value().success);
}

TEST_F(PrefixManagerTestFixture, RemoveInvalidType) {
  EXPECT_TRUE(prefixManagerClient->addPrefixes({prefixEntry1}).value().success);
  EXPECT_TRUE(prefixManagerClient->addPrefixes({prefixEntry2}).value().success);

  // Verify that prefix type has to match for withdrawing prefix
  auto prefixEntryError = prefixEntry1;
  prefixEntryError.type = thrift::PrefixType::PREFIX_ALLOCATOR;

  auto resp1 =
      prefixManagerClient->withdrawPrefixes({prefixEntryError, prefixEntry2});
  EXPECT_FALSE(resp1.value().success);

  // Verify that all prefixes are still present
  auto resp2 = prefixManagerClient->getPrefixes();
  EXPECT_TRUE(resp2.value().success);
  EXPECT_EQ(2, resp2.value().prefixes.size());

  // Verify withdrawing of multiple prefixes
  auto resp3 =
      prefixManagerClient->withdrawPrefixes({prefixEntry1, prefixEntry2});
  EXPECT_TRUE(resp3.value().success);

  // Verify that there are no prefixes
  auto resp4 = prefixManagerClient->getPrefixes();
  EXPECT_TRUE(resp4.value().success);
  EXPECT_EQ(0, resp4.value().prefixes.size());
}

TEST_F(PrefixManagerTestFixture, VerifyKvStore) {
  prefixManagerClient->addPrefixes({prefixEntry1});

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue = kvStoreClient->getKey("prefix:node-1");
  EXPECT_FALSE(maybeValue.hasError());
  auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue.value().value.value(), serializer);
  EXPECT_EQ(db.thisNodeName, "node-1");
  EXPECT_EQ(db.prefixEntries.size(), 1);

  prefixManagerClient->withdrawPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  prefixManagerClient->addPrefixes({prefixEntry3});
  prefixManagerClient->addPrefixes({prefixEntry4});
  prefixManagerClient->addPrefixes({prefixEntry5});
  prefixManagerClient->addPrefixes({prefixEntry6});
  prefixManagerClient->addPrefixes({prefixEntry7});
  prefixManagerClient->addPrefixes({prefixEntry8});
  prefixManagerClient->addPrefixes({ephemeralPrefixEntry9});

  /* Verify that before throttle expires, we don't see any update */
  std::this_thread::sleep_for(Constants::kPrefixMgrKvThrottleTimeout / 2);
  auto maybeValue1 = kvStoreClient->getKey("prefix:node-1");
  EXPECT_FALSE(maybeValue1.hasError());
  auto db1 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue1.value().value.value(), serializer);
  EXPECT_EQ(db1.thisNodeName, "node-1");
  EXPECT_EQ(db1.prefixEntries.size(), 1);

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue2 = kvStoreClient->getKey("prefix:node-1");
  EXPECT_FALSE(maybeValue2.hasError());
  auto db2 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue2.value().value.value(), serializer);
  EXPECT_EQ(db2.thisNodeName, "node-1");
  EXPECT_EQ(db2.prefixEntries.size(), 8);

  // now make a change and check again
  prefixManagerClient->withdrawPrefixesByType(thrift::PrefixType::DEFAULT);

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue3 = kvStoreClient->getKey("prefix:node-1");
  EXPECT_FALSE(maybeValue3.hasError());
  auto db3 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue3.value().value.value(), serializer);
  EXPECT_EQ(db3.thisNodeName, "node-1");
  EXPECT_EQ(db3.prefixEntries.size(), 5);
}

TEST_F(PrefixManagerTestFixture, CheckReload) {
  prefixManagerClient->addPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  prefixManagerClient->addPrefixes({ephemeralPrefixEntry9});
  // spin up a new PrefixManager add verify that it loads the config
  auto prefixManager2 = std::make_unique<PrefixManager>(
      "node-2",
      std::string{kPrefixManagerGlobalCmdUrl + "2"},
      PersistentStoreUrl{kConfigStoreUrl},
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{"prefix:"},
      false /* prefix-mananger perf measurement */,
      std::chrono::seconds(0),
      Constants::kKvStoreDbTtl,
      context);

  auto prefixManagerThread2 = std::make_unique<std::thread>([&]() {
    LOG(INFO) << "PrefixManager thread starting";
    prefixManager2->run();
    LOG(INFO) << "PrefixManager thread finishing";
  });
  prefixManager2->waitUntilRunning();

  auto prefixManagerClient2 = std::make_unique<PrefixManagerClient>(
      PrefixManagerLocalCmdUrl{prefixManager2->inprocCmdUrl}, context);

  // verify that the new manager has only persistent prefixes.
  // Ephemeral prefixes will not be reloaded.
  EXPECT_TRUE(
      prefixManagerClient2->withdrawPrefixes({prefixEntry1}).value().success);
  EXPECT_TRUE(
      prefixManagerClient2->withdrawPrefixes({prefixEntry2}).value().success);
  EXPECT_FALSE(prefixManagerClient2->withdrawPrefixes({ephemeralPrefixEntry9})
                   .value()
                   .success);
  // cleanup
  prefixManager2->stop();
  prefixManagerThread2->join();
}

TEST_F(PrefixManagerTestFixture, GetPrefixes) {
  prefixManagerClient->addPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  prefixManagerClient->addPrefixes({prefixEntry3});
  prefixManagerClient->addPrefixes({prefixEntry4});
  prefixManagerClient->addPrefixes({prefixEntry5});
  prefixManagerClient->addPrefixes({prefixEntry6});
  prefixManagerClient->addPrefixes({prefixEntry7});

  auto resp1 = prefixManagerClient->getPrefixes();
  EXPECT_TRUE(resp1.value().success);
  auto& prefixes1 = resp1.value().prefixes;
  EXPECT_EQ(7, prefixes1.size());
  EXPECT_NE(
      std::find(prefixes1.begin(), prefixes1.end(), prefixEntry4),
      prefixes1.end());
  EXPECT_EQ(
      std::find(prefixes1.begin(), prefixes1.end(), prefixEntry8),
      prefixes1.end());

  auto resp2 =
      prefixManagerClient->getPrefixesByType(thrift::PrefixType::DEFAULT);
  EXPECT_TRUE(resp2.value().success);
  auto& prefixes2 = resp2.value().prefixes;
  EXPECT_EQ(4, prefixes2.size());
  EXPECT_NE(
      std::find(prefixes2.begin(), prefixes2.end(), prefixEntry3),
      prefixes2.end());
  EXPECT_EQ(
      std::find(prefixes2.begin(), prefixes2.end(), prefixEntry2),
      prefixes2.end());

  auto resp3 =
      prefixManagerClient->withdrawPrefixesByType(thrift::PrefixType::DEFAULT);
  EXPECT_TRUE(resp3.value().success);

  auto resp4 =
      prefixManagerClient->getPrefixesByType(thrift::PrefixType::DEFAULT);
  EXPECT_TRUE(resp4.value().success);
  EXPECT_EQ(0, resp4.value().prefixes.size());
}

TEST_F(PrefixManagerTestFixture, PrefixAddCount) {
  auto count0 = prefixManager->getPrefixAddCounter();
  EXPECT_EQ(0, count0);

  prefixManagerClient->addPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  prefixManagerClient->addPrefixes({prefixEntry3});

  auto count1 = prefixManager->getPrefixAddCounter();
  EXPECT_EQ(3, count1);

  prefixManagerClient->addPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  auto count2 = prefixManager->getPrefixAddCounter();
  EXPECT_EQ(5, count2);

  prefixManagerClient->withdrawPrefixes({prefixEntry1});
  auto count3 = prefixManager->getPrefixAddCounter();
  EXPECT_EQ(5, count3);
}

TEST_F(PrefixManagerTestFixture, PrefixWithdrawCount) {
  auto count0 = prefixManager->getPrefixWithdrawCounter();
  EXPECT_EQ(0, count0);

  prefixManagerClient->withdrawPrefixes({prefixEntry1});
  auto count1 = prefixManager->getPrefixWithdrawCounter();
  EXPECT_EQ(0, count1);

  prefixManagerClient->addPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  prefixManagerClient->addPrefixes({prefixEntry3});

  auto count2 = prefixManager->getPrefixWithdrawCounter();
  EXPECT_EQ(0, count2);

  prefixManagerClient->withdrawPrefixes({prefixEntry1});
  auto count3 = prefixManager->getPrefixWithdrawCounter();
  EXPECT_EQ(1, count3);

  prefixManagerClient->withdrawPrefixes({prefixEntry4});
  auto count4 = prefixManager->getPrefixWithdrawCounter();
  EXPECT_EQ(1, count4);

  prefixManagerClient->withdrawPrefixes({prefixEntry1});
  prefixManagerClient->withdrawPrefixes({prefixEntry2});
  auto count5 = prefixManager->getPrefixWithdrawCounter();
  EXPECT_EQ(2, count5);
}

TEST(PrefixManagerTest, HoldTimeout) {
  fbzmq::Context context;

  // spin up a config store
  auto configStore = std::make_unique<PersistentStore>(
      "1",
      folly::sformat(
          "/tmp/pm_ut_config_store.bin.{}",
          std::hash<std::thread::id>{}(std::this_thread::get_id())),
      PersistentStoreUrl{kConfigStoreUrl},
      context);
  std::thread configStoreThread([&]() noexcept {
    LOG(INFO) << "ConfigStore thread starting";
    configStore->run();
    LOG(INFO) << "ConfigStore thread finishing";
  });
  configStore->waitUntilRunning();

  // spin up a kvstore
  auto kvStoreWrapper = std::make_unique<KvStoreWrapper>(
      context,
      "test_store1",
      std::chrono::seconds(1) /* db sync interval */,
      std::chrono::seconds(600) /* counter submit interval */,
      std::unordered_map<std::string, thrift::PeerSpec>{});
  kvStoreWrapper->run();
  LOG(INFO) << "The test KV store is running";

  // start a prefix manager with timeout
  const std::chrono::seconds holdTime{2};
  const auto startTime = std::chrono::steady_clock::now();
  auto prefixManager = std::make_unique<PrefixManager>(
      "node-1",
      std::string{kPrefixManagerGlobalCmdUrl},
      PersistentStoreUrl{kConfigStoreUrl},
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{"prefix:"},
      false /* prefix-mananger perf measurement */,
      holdTime,
      Constants::kKvStoreDbTtl,
      context);
  std::thread prefixManagerThread([&]() {
    LOG(INFO) << "PrefixManager thread starting";
    prefixManager->run();
    LOG(INFO) << "PrefixManager thread finishing";
  });
  prefixManager->waitUntilRunning();

  // We must receive publication after holdTime
  auto publication = kvStoreWrapper->recvPublication(holdTime * 2);
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime);
  CHECK_GE(
      elapsedTime.count(),
      std::chrono::duration_cast<std::chrono::milliseconds>(holdTime).count());
  CHECK_EQ(1, publication.keyVals.size());
  CHECK_EQ(1, publication.keyVals.count("prefix:node-1"));

  // Stop the test
  prefixManager->stop();
  prefixManagerThread.join();
  kvStoreWrapper->stop();
  configStore->stop();
  configStoreThread.join();
}

// Verify that persist store is updated only when
// non-ephemeral types are effected
TEST_F(PrefixManagerTestFixture, CheckPersistStoreUpdate) {
  ASSERT_EQ(0, configStore->getNumOfDbWritesToDisk());
  // Verify that any action on persistent entries leads to update of store
  prefixManagerClient->addPrefixes({prefixEntry1, prefixEntry2, prefixEntry3});
  // 3 prefixes leads to 1 write
  ASSERT_EQ(1, configStore->getNumOfDbWritesToDisk());

  prefixManagerClient->withdrawPrefixes({prefixEntry1});
  ASSERT_EQ(2, configStore->getNumOfDbWritesToDisk());

  prefixManagerClient->syncPrefixesByType(
      thrift::PrefixType::PREFIX_ALLOCATOR, {prefixEntry2, prefixEntry4});
  ASSERT_EQ(3, configStore->getNumOfDbWritesToDisk());

  prefixManagerClient->withdrawPrefixesByType(
      thrift::PrefixType::PREFIX_ALLOCATOR);
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  // Verify that any actions on ephemeral entries does not lead to update of
  // store
  prefixManagerClient->addPrefixes(
      {ephemeralPrefixEntry9, ephemeralPrefixEntry10});
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  prefixManagerClient->withdrawPrefixes({ephemeralPrefixEntry9});
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  prefixManagerClient->syncPrefixesByType(
      thrift::PrefixType::BGP, {ephemeralPrefixEntry10});
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  prefixManagerClient->withdrawPrefixesByType(thrift::PrefixType::BGP);
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());
}

// Verify that persist store is update properly when both persistent
// and ephemeral entries are mixed for same prefix type
TEST_F(PrefixManagerTestFixture, CheckEphemeralAndPersistentUpdate) {
  ASSERT_EQ(0, configStore->getNumOfDbWritesToDisk());
  // Verify that any action on persistent entries leads to update of store
  prefixManagerClient->addPrefixes(
      {persistentPrefixEntry9, ephemeralPrefixEntry10});
  ASSERT_EQ(1, configStore->getNumOfDbWritesToDisk());

  // Change persistance characterstic. Expect disk update
  prefixManagerClient->syncPrefixesByType(
      thrift::PrefixType::BGP,
      {ephemeralPrefixEntry9, persistentPrefixEntry10});
  ASSERT_EQ(2, configStore->getNumOfDbWritesToDisk());

  // Only ephemeral entry withdrawn, so no update to disk
  prefixManagerClient->withdrawPrefixes({ephemeralPrefixEntry9});
  ASSERT_EQ(2, configStore->getNumOfDbWritesToDisk());

  // Persistent entry withdrawn, expect update to disk
  prefixManagerClient->withdrawPrefixes({persistentPrefixEntry10});
  ASSERT_EQ(3, configStore->getNumOfDbWritesToDisk());

  // Restore the state to mix of ephemeral and persistent of a type
  prefixManagerClient->addPrefixes(
      {persistentPrefixEntry9, ephemeralPrefixEntry10});
  ASSERT_EQ(4, configStore->getNumOfDbWritesToDisk());

  // Verify that withdraw by type, updates disk
  prefixManagerClient->withdrawPrefixesByType(thrift::PrefixType::BGP);
  ASSERT_EQ(5, configStore->getNumOfDbWritesToDisk());

  // Restore the state to mix of ephemeral and persistent of a type
  prefixManagerClient->addPrefixes(
      {persistentPrefixEntry9, ephemeralPrefixEntry10});
  ASSERT_EQ(6, configStore->getNumOfDbWritesToDisk());

  // Verify that entry in DB being deleted is persistent so file is update
  prefixManagerClient->syncPrefixesByType(
      thrift::PrefixType::BGP, {ephemeralPrefixEntry10});
  ASSERT_EQ(7, configStore->getNumOfDbWritesToDisk());
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
