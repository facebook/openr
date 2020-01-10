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

#include <openr/common/Constants.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/prefix-manager/PrefixManager.h>
#include <openr/prefix-manager/PrefixManagerClient.h>

using namespace openr;

using apache::thrift::CompactSerializer;

namespace {

const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");
const auto addr5 = toIpPrefix("ffff:10:1:5::/64");
const auto addr6 = toIpPrefix("ffff:10:2:6::/64");
const auto addr7 = toIpPrefix("ffff:10:3:7::0/64");
const auto addr8 = toIpPrefix("ffff:10:4:8::/64");
const auto addr9 = toIpPrefix("ffff:10:4:9::/64");
const auto addr10 = toIpPrefix("ffff:10:4:10::/64");

const auto prefixEntry1 = createPrefixEntry(addr1, thrift::PrefixType::DEFAULT);
const auto prefixEntry2 =
    createPrefixEntry(addr2, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry3 = createPrefixEntry(addr3, thrift::PrefixType::DEFAULT);
const auto prefixEntry4 =
    createPrefixEntry(addr4, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry5 = createPrefixEntry(addr5, thrift::PrefixType::DEFAULT);
const auto prefixEntry6 =
    createPrefixEntry(addr6, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto prefixEntry7 = createPrefixEntry(addr7, thrift::PrefixType::DEFAULT);
const auto prefixEntry8 =
    createPrefixEntry(addr8, thrift::PrefixType::PREFIX_ALLOCATOR);
const auto ephemeralPrefixEntry9 = createPrefixEntry(
    addr9,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    true);

const auto persistentPrefixEntry9 = createPrefixEntry(
    addr9,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    false);
const auto ephemeralPrefixEntry10 = createPrefixEntry(
    addr10,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    true);
const auto persistentPrefixEntry10 = createPrefixEntry(
    addr10,
    thrift::PrefixType::BGP,
    {},
    thrift::PrefixForwardingType::IP,
    thrift::PrefixForwardingAlgorithm::SP_ECMP,
    false);
} // namespace

class PrefixManagerTestFixture : public testing::TestWithParam<bool> {
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
        context,
        std::chrono::milliseconds(0),
        std::chrono::milliseconds(0),
        true);

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
        PersistentStoreUrl{configStore->inprocCmdUrl},
        KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
        KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
        MonitorSubmitUrl{"inproc://monitor_submit"},
        PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
        perPrefixKeys_ /* per prefix keys */,
        true /* prefix-mananger perf measurement */,
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
    PersistentStoreClient configStoreClient{
        PersistentStoreUrl{configStore->inprocCmdUrl}, context};
    configStoreClient.erase("prefix-manager-config");

    // stop config store
    configStore->stop();
    configStoreThread->join();

    // stop the kvStore
    kvStoreWrapper->stop();
    LOG(INFO) << "The test KV store is stopped";
  }

  // In case of separate IP prefix keys, collect all the prefix Entries
  // (advertised from a specific node) and return as a list
  std::vector<thrift::PrefixEntry>
  getPrefixDb(const std::string& keyPrefix) {
    std::vector<thrift::PrefixEntry> prefixEntries{};
    auto marker = PrefixDbMarker{Constants::kPrefixDbMarker.toString()};
    auto keyPrefixDbs = kvStoreClient->dumpAllWithPrefix(keyPrefix);
    for (const auto& pkey : keyPrefixDbs.value()) {
      if (pkey.first.find(marker) == 0) {
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            pkey.second.value.value(), serializer);
        // skip prefixes marked for delete
        if (!prefixDb.deletePrefix) {
          prefixEntries.insert(
              prefixEntries.begin(),
              prefixDb.prefixEntries.begin(),
              prefixDb.prefixEntries.end());
        }
      }
    }
    return prefixEntries;
  }

  fbzmq::Context context;

  OpenrEventBase evl;

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
  const bool perPrefixKeys_ = GetParam();
};

INSTANTIATE_TEST_CASE_P(
    PrefixManagerInstance, PrefixManagerTestFixture, ::testing::Bool());

TEST_P(PrefixManagerTestFixture, AddRemovePrefix) {
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

TEST_P(PrefixManagerTestFixture, RemoveUpdateType) {
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

TEST_P(PrefixManagerTestFixture, RemoveInvalidType) {
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

TEST_P(PrefixManagerTestFixture, VerifyKvStore) {
  prefixManagerClient->addPrefixes({prefixEntry1});

  std::string keyStr{"prefix:node-1"};

  if (perPrefixKeys_) {
    auto prefixKey = PrefixKey(
        "node-1",
        folly::IPAddress::createNetwork(toString(prefixEntry1.prefix)),
        thrift::KvStore_constants::kDefaultArea());

    keyStr = prefixKey.getPrefixKey();
  }

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue.hasError());
  auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue.value().value.value(), serializer);
  EXPECT_EQ(db.thisNodeName, "node-1");
  EXPECT_EQ(db.prefixEntries.size(), 1);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }

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
  auto maybeValue1 = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue1.hasError());
  auto db1 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue1.value().value.value(), serializer);
  auto prefixDb = getPrefixDb("prefix:node-1");
  EXPECT_EQ(prefixDb.size(), 1);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue2 = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue2.hasError());
  auto db2 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue2.value().value.value(), serializer);
  prefixDb = getPrefixDb("prefix:node-1");
  EXPECT_EQ(prefixDb.size(), 8);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }

  // now make a change and check again
  prefixManagerClient->withdrawPrefixesByType(thrift::PrefixType::DEFAULT);

  // Wait for throttled update to announce to kvstore
  std::this_thread::sleep_for(2 * Constants::kPrefixMgrKvThrottleTimeout);
  auto maybeValue3 = kvStoreClient->getKey(keyStr);
  EXPECT_FALSE(maybeValue3.hasError());
  auto db3 = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
      maybeValue3.value().value.value(), serializer);
  prefixDb = getPrefixDb("prefix:node-1");
  EXPECT_EQ(prefixDb.size(), 5);
  ASSERT_TRUE(db.perfEvents.hasValue());
  ASSERT_FALSE(db.perfEvents->events.empty());
  {
    const auto& perfEvent = db.perfEvents->events.back();
    EXPECT_EQ("UPDATE_KVSTORE_THROTTLED", perfEvent.eventDescr);
    EXPECT_EQ("node-1", perfEvent.nodeName);
    EXPECT_LT(0, perfEvent.unixTs); // Non zero timestamp
  }
}

/**
 * Test prefix advertisement in KvStore with multiple clients.
 * NOTE: Priority LOOPBACK > DEFAULT > BGP
 * 1. Inject prefix1 with client-bgp - Verify KvStore
 * 2. Inject prefix1 with client-loopback and client-default - Verify KvStore
 * 3. Withdraw prefix1 with client-loopback - Verify KvStore
 * 4. Withdraw prefix1 with client-bgp, client-default - Verify KvStore
 */
TEST_P(PrefixManagerTestFixture, VerifyKvStoreMultipleClients) {
  const auto loopback_prefix =
      createPrefixEntry(addr1, thrift::PrefixType::LOOPBACK);
  const auto default_prefix =
      createPrefixEntry(addr1, thrift::PrefixType::DEFAULT);
  const auto bgp_prefix = createPrefixEntry(addr1, thrift::PrefixType::BGP);

  std::string keyStr{"prefix:node-1"};
  if (perPrefixKeys_) {
    keyStr = PrefixKey(
                 "node-1",
                 toIPNetwork(addr1),
                 thrift::KvStore_constants::kDefaultArea())
                 .getPrefixKey();
  }

  // Synchronization primitive
  folly::Baton baton;
  folly::Optional<thrift::PrefixEntry> expectedPrefix;
  bool gotExpected = true;

  kvStoreClient->subscribeKey(
      keyStr,
      [&](std::string const& _, folly::Optional<thrift::Value> val) mutable {
        ASSERT_TRUE(val.hasValue());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            val->value.value(), serializer);
        EXPECT_EQ(db.thisNodeName, "node-1");
        if (expectedPrefix.hasValue() and db.prefixEntries.size() != 0) {
          // we should always be advertising one prefix until we withdraw all
          EXPECT_EQ(db.prefixEntries.size(), 1);
          EXPECT_EQ(expectedPrefix, db.prefixEntries.at(0));
          gotExpected = true;
        } else {
          EXPECT_TRUE(not perPrefixKeys_ or db.deletePrefix);
          EXPECT_TRUE(not perPrefixKeys_ or db.prefixEntries.size() == 1);
        }

        // Signal verification
        if (gotExpected) {
          baton.post();
        }
      });

  // Start event loop in it's own thread
  std::thread evlThread([&]() { evl.run(); });
  evl.waitUntilRunning();

  //
  // 1. Inject prefix1 with client-bgp - Verify KvStore
  //
  expectedPrefix = bgp_prefix;
  gotExpected = false;
  prefixManagerClient->addPrefixes({bgp_prefix});
  baton.wait();
  baton.reset();

  //
  // 2. Inject prefix1 with client-loopback and client-default - Verify KvStore
  //
  expectedPrefix = loopback_prefix; // lowest client-id will win
  gotExpected = false;
  prefixManagerClient->addPrefixes({loopback_prefix, default_prefix});
  baton.wait();
  baton.reset();

  //
  // 3. Withdraw prefix1 with client-loopback - Verify KvStore
  //
  expectedPrefix = default_prefix;
  gotExpected = false;
  prefixManagerClient->withdrawPrefixes({loopback_prefix});
  baton.wait();
  baton.reset();

  //
  // 4. Withdraw prefix1 with client-bgp, client-default - Verify KvStore
  //
  expectedPrefix = folly::none;
  gotExpected = true;
  prefixManagerClient->withdrawPrefixes({bgp_prefix, default_prefix});
  baton.wait();
  baton.reset();

  // Nuke test environment
  evl.stop();
  evlThread.join();
}

/**
 * test to check prefix key add, withdraw does not trigger update for all
 * the prefixes managed by the prefix manager. This test does not apply to
 * the old key format
 */
TEST_P(PrefixManagerTestFixture, PrefixKeyUpdates) {
  int waitDuration{0};
  // test only if 'create ip prefixes' is enabled
  if (!perPrefixKeys_) {
    return;
  }

  auto prefixKey1 = PrefixKey(
      "node-1",
      folly::IPAddress::createNetwork(toString(prefixEntry1.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  auto prefixKey2 = PrefixKey(
      "node-1",
      folly::IPAddress::createNetwork(toString(prefixEntry2.prefix)),
      thrift::KvStore_constants::kDefaultArea());

  // Schedule callback to set keys from client1 (this will be executed first)
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 0), [&]() noexcept {
        prefixManagerClient->addPrefixes({prefixEntry1});
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);
      });

  // add another key
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { prefixManagerClient->addPrefixes({prefixEntry2}); });

  // version of first key should still be 1
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 4 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        EXPECT_EQ(maybeValue2.value().version, 1);
      });

  // withdraw prefixEntry2
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        prefixManagerClient->withdrawPrefixes({prefixEntry2});
      });

  // version of prefixEntry1 should still be 1
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);

        // verify key is withdrawn
        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue2.value().value.value(), serializer);
        EXPECT_NE(db.prefixEntries.size(), 0);
        EXPECT_TRUE(db.deletePrefix);
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { evl.stop(); });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "main loop starting.";
    evl.run();
    LOG(INFO) << "main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();
}

/**
 * Test prefix key subscription callback from Kvstore client.
 * The test verifies the callback takes the action that reflects the current
 * state of prefix in the prefix manager (either exists or does not exist) and
 * appropriately udpates Kvstore
 */
TEST_P(PrefixManagerTestFixture, PrefixKeySubscribtion) {
  int waitDuration{0};
  int keyVersion{0};
  std::string prefixKeyStr{"prefix:node-1"};
  const auto prefixEntry =
      createPrefixEntry(toIpPrefix("5001::/64"), thrift::PrefixType::DEFAULT);
  auto prefixKey = PrefixKey(
      "node-1",
      folly::IPAddress::createNetwork(toString(prefixEntry.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  if (perPrefixKeys_) {
    prefixKeyStr = prefixKey.getPrefixKey();
  }

  // Schedule callback to set keys from client1 (this will be executed first)
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 0), [&]() noexcept {
        prefixManagerClient->addPrefixes({prefixEntry});
      });

  // Wait for throttled update to announce to kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        keyVersion = maybeValue.value().version;
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(db.thisNodeName, "node-1");
        EXPECT_EQ(db.prefixEntries.size(), 1);
        EXPECT_EQ(db.prefixEntries[0], prefixEntry);
      });

  thrift::PrefixDatabase emptyPrefxDb;
  emptyPrefxDb.thisNodeName = "node-1";
  emptyPrefxDb.prefixEntries = {};
  const auto emptyPrefxDbStr =
      fbzmq::util::writeThriftObjStr(emptyPrefxDb, serializer);

  // increment the key version in kvstore and set empty value. kvstoreClient
  // will detect value changed, and retain the value present in peristent DB,
  // and advertise with higher key version.
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 10), [&]() noexcept {
        kvStoreClient->setKey(
            prefixKeyStr,
            emptyPrefxDbStr,
            keyVersion + 1,
            Constants::kKvStoreDbTtl);
      });

  // Wait for throttled update to announce to kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(maybeValue.value().version, keyVersion + 2);
        EXPECT_EQ(db.thisNodeName, "node-1");
        EXPECT_EQ(db.prefixEntries.size(), 1);
        EXPECT_EQ(db.prefixEntries[0], prefixEntry);
      });

  // Clear key from prefix DB map, which will delete key from persistent
  // store and update kvstore with empty prefix entry list
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { prefixManagerClient->withdrawPrefixes({prefixEntry}); });

  // verify key is withdrawn from kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(maybeValue.value().version, keyVersion + 3);
        EXPECT_EQ(db.thisNodeName, "node-1");
        // delete prefix must be set to TRUE, applies only when per prefix key
        // is enabled
        if (perPrefixKeys_) {
          EXPECT_NE(db.prefixEntries.size(), 0);
          EXPECT_TRUE(db.deletePrefix);
        }
      });

  thrift::PrefixDatabase nonEmptyPrefxDb;
  nonEmptyPrefxDb.thisNodeName = "node-1";
  nonEmptyPrefxDb.prefixEntries = {prefixEntry};
  const auto nonEmptyPrefxDbStr =
      fbzmq::util::writeThriftObjStr(nonEmptyPrefxDb, serializer);

  // Insert same key in kvstore with any higher version, and non empty value
  // Prefix manager should get the update and re-advertise with empty Prefix
  // with higher key version.
  int staleKeyVersion{100};
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        kvStoreClient->setKey(
            prefixKeyStr,
            nonEmptyPrefxDbStr,
            staleKeyVersion,
            Constants::kKvStoreDbTtl);
      });

  // prefix manager will override the key inserted above with higher key
  // version and empty prefix DB
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasError());
        auto db = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            maybeValue.value().value.value(), serializer);
        EXPECT_EQ(maybeValue.value().version, staleKeyVersion + 1);
        EXPECT_EQ(db.thisNodeName, "node-1");
        // delete prefix must be set to TRUE, applies only when per prefix key
        // is enabled
        if (perPrefixKeys_) {
          EXPECT_NE(db.prefixEntries.size(), 0);
          EXPECT_TRUE(db.deletePrefix);
        }
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { evl.stop(); });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "main loop starting.";
    evl.run();
    LOG(INFO) << "main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();
}

TEST_P(PrefixManagerTestFixture, PrefixWithdrawExpiry) {
  int waitDuration{0};

  if (!perPrefixKeys_) {
    return;
  }

  std::chrono::milliseconds ttl{100};
  // spin up a new PrefixManager add verify that it loads the config
  auto prefixManager2 = std::make_unique<PrefixManager>(
      "node-2",
      PersistentStoreUrl{configStore->inprocCmdUrl},
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      perPrefixKeys_ /* create IP prefix keys */,
      false /* prefix-mananger perf measurement */,
      std::chrono::seconds(0),
      ttl,
      context);

  auto prefixManagerThread2 = std::make_unique<std::thread>([&]() {
    LOG(INFO) << "PrefixManager thread starting";
    prefixManager2->run();
    LOG(INFO) << "PrefixManager thread finishing";
  });
  prefixManager2->waitUntilRunning();

  auto prefixManagerClient2 = std::make_unique<PrefixManagerClient>(
      PrefixManagerLocalCmdUrl{prefixManager2->inprocCmdUrl}, context);

  auto prefixKey1 = PrefixKey(
      "node-2",
      folly::IPAddress::createNetwork(toString(prefixEntry1.prefix)),
      thrift::KvStore_constants::kDefaultArea());
  auto prefixKey2 = PrefixKey(
      "node-2",
      folly::IPAddress::createNetwork(toString(prefixEntry2.prefix)),
      thrift::KvStore_constants::kDefaultArea());

  // insert two prefixes
  evl.scheduleTimeout(
      std::chrono::milliseconds(waitDuration += 0), [&]() noexcept {
        prefixManagerClient2->addPrefixes({prefixEntry1});
        prefixManagerClient2->addPrefixes({prefixEntry2});
      });

  // check both prefixes are in kvstore
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue.hasValue());
        EXPECT_EQ(maybeValue.value().version, 1);

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        EXPECT_EQ(maybeValue2.value().version, 1);
      });

  // withdraw prefixEntry1
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept {
        prefixManagerClient2->withdrawPrefixes({prefixEntry1});
      });

  // check prefix entry1 should have been expired, prefix 2 should be there
  // with same version
  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration +=
          2 * Constants::kPrefixMgrKvThrottleTimeout.count() + ttl.count()),
      [&]() noexcept {
        auto prefixKeyStr = prefixKey1.getPrefixKey();
        auto maybeValue = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_FALSE(maybeValue.hasValue());

        prefixKeyStr = prefixKey2.getPrefixKey();
        auto maybeValue2 = kvStoreClient->getKey(prefixKeyStr);
        EXPECT_TRUE(maybeValue2.hasValue());
        EXPECT_EQ(maybeValue2.value().version, 1);
      });

  evl.scheduleTimeout(
      std::chrono::milliseconds(
          waitDuration += 2 * Constants::kPrefixMgrKvThrottleTimeout.count()),
      [&]() noexcept { evl.stop(); });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "main loop starting.";
    evl.run();
    LOG(INFO) << "main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();

  // cleanup
  prefixManager2->stop();
  prefixManagerThread2->join();
}

TEST_P(PrefixManagerTestFixture, CheckReload) {
  prefixManagerClient->addPrefixes({prefixEntry1});
  prefixManagerClient->addPrefixes({prefixEntry2});
  prefixManagerClient->addPrefixes({ephemeralPrefixEntry9});
  // spin up a new PrefixManager add verify that it loads the config
  auto prefixManager2 = std::make_unique<PrefixManager>(
      "node-2",
      PersistentStoreUrl{configStore->inprocCmdUrl},
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      perPrefixKeys_ /* create IP prefix keys */,
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

TEST_P(PrefixManagerTestFixture, GetPrefixes) {
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

TEST_P(PrefixManagerTestFixture, PrefixAddCount) {
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

TEST_P(PrefixManagerTestFixture, PrefixWithdrawCount) {
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
      context,
      Constants::kPersistentStoreInitialBackoff,
      Constants::kPersistentStoreMaxBackoff,
      true);
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
      PersistentStoreUrl{configStore->inprocCmdUrl},
      KvStoreLocalCmdUrl{kvStoreWrapper->localCmdUrl},
      KvStoreLocalPubUrl{kvStoreWrapper->localPubUrl},
      MonitorSubmitUrl{"inproc://monitor_submit"},
      PrefixDbMarker{Constants::kPrefixDbMarker.toString()},
      false /* create IP prefix keys */,
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
TEST_P(PrefixManagerTestFixture, CheckPersistStoreUpdate) {
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
TEST_P(PrefixManagerTestFixture, CheckEphemeralAndPersistentUpdate) {
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
