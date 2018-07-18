/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sodium.h>
#include <thread>
#include <unordered_set>

#include <openr/common/Util.h>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Optional.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/kvstore/KvStoreWrapper.h>

using namespace std;
using namespace folly;

using namespace openr;

using apache::thrift::CompactSerializer;

using namespace std::chrono_literals;

namespace {

// the size of the value string
const uint32_t kValueStrSize = 64;
// max packet inter-arrival time (can't be chrono)
const uint32_t kReqTimeoutMs = 4000;
// interval for periodic syncs
const std::chrono::seconds kSyncInterval(1);
// maximum timeout for single request for sync
const std::chrono::milliseconds kSyncReqTimeout(1000);
// maximum timeout waiting for all peers to respond to sync request
const std::chrono::milliseconds kSyncMaxWaitTime(1000);

const std::chrono::milliseconds kTtl{1000};
} // namespace

//
// Three-store fixture to test dumpAllWithPrefixMultiple*
//
class MultipleStoreFixture : public ::testing::Test {
 public:
  void
  SetUp() override {
    auto makeStore = [this](std::string nodeId) {
      const auto peers = std::unordered_map<std::string, thrift::PeerSpec>{};
      return std::make_shared<KvStoreWrapper>(
          context,
          nodeId,
          60s /* db sync interval */,
          600s /* counter submit interval */,
          peers);
    };

    store1 = makeStore(node1);
    store2 = makeStore(node2);
    store3 = makeStore(node3);

    store1->run();
    store2->run();
    store3->run();

    // Create and initialize kvstore-clients
    client1 = std::make_shared<KvStoreClient>(
        context, &evl, node1, store1->localCmdUrl, store1->localPubUrl);

    client2 = std::make_shared<KvStoreClient>(
        context, &evl, node2, store2->localCmdUrl, store2->localPubUrl);

    client3 = std::make_shared<KvStoreClient>(
        context, &evl, node3, store3->localCmdUrl, store3->localPubUrl);

    urls = {fbzmq::SocketUrl{store1->localCmdUrl},
            fbzmq::SocketUrl{store2->localCmdUrl},
            fbzmq::SocketUrl{store3->localCmdUrl}};
  }

  void
  TearDown() override {
    store1->stop();
    store2->stop();
    store3->stop();
  }

  fbzmq::Context context;

  fbzmq::ZmqEventLoop evl;

  std::shared_ptr<KvStoreWrapper> store1, store2, store3;

  std::shared_ptr<KvStoreClient> client1, client2, client3;

  const std::string node1{"test_store1"}, node2{"test_store2"},
      node3{"test_store3"};

  apache::thrift::CompactSerializer serializer;

  std::vector<fbzmq::SocketUrl> urls;
};

/**
 * All stores for multi-get are failing
 */
TEST_F(MultipleStoreFixture, dumpWithPrefixAndParseMultiple_allStoresDown) {
  store1->stop();
  store2->stop();
  store3->stop();

  auto maybe = KvStoreClient::dumpAllWithPrefixMultiple(
      context, urls, "test_", 1000ms, 192);

  ASSERT_FALSE(maybe.first.hasValue());
  // ALL url should be unreachable
  EXPECT_EQ(urls.size(), maybe.second.size());
}

/**
 * Merge different keys from three stores
 */
TEST_F(MultipleStoreFixture, dumpWithPrefixMultiple_differentKeys) {
  //
  // Submit three values in three different stores
  //
  evl.runInEventLoop([&]() noexcept {
    thrift::Value value;
    value.version = 1;
    {
      value.value = "test_value1";
      client1->setKey(
          "test_key1", fbzmq::util::writeThriftObjStr(value, serializer), 100);
    }
    {
      value.value = "test_value2";
      client2->setKey(
          "test_key2", fbzmq::util::writeThriftObjStr(value, serializer), 200);
    }
    {
      value.value = "test_value3";
      client3->setKey(
          "test_key3", fbzmq::util::writeThriftObjStr(value, serializer), 300);
    }

    evl.stop();
  });

  evl.run();

  auto maybe = KvStoreClient::dumpAllWithPrefixMultipleAndParse<thrift::Value>(
      context, urls, "test_");

  ASSERT_TRUE(maybe.first.hasValue());

  {
    auto dump = maybe.first.value();
    EXPECT_EQ(3, dump.size());
    EXPECT_EQ("test_value1", dump["test_key1"].value);
    EXPECT_EQ("test_value2", dump["test_key2"].value);
    EXPECT_EQ("test_value3", dump["test_key3"].value);
  }
}

/**
 * Merge same key with diff. values based on versions
 */
TEST_F(
    MultipleStoreFixture,
    dumpAllWithPrefixMultipleAndParse_sameKeysDiffValues) {
  //
  // Submit three values in three different stores
  //
  evl.runInEventLoop([&]() noexcept {
    thrift::Value value;
    {
      value.value = "test_value1";
      client1->setKey(
          "test_key", fbzmq::util::writeThriftObjStr(value, serializer), 300);
    }
    {
      value.value = "test_value2";
      client2->setKey(
          "test_key", fbzmq::util::writeThriftObjStr(value, serializer), 200);
    }
    {
      value.value = "test_value3";
      client3->setKey(
          "test_key", fbzmq::util::writeThriftObjStr(value, serializer), 100);
    }

    evl.stop();
  });

  evl.run();

  auto maybe = KvStoreClient::dumpAllWithPrefixMultipleAndParse<thrift::Value>(
      context, urls, "test_");

  ASSERT_TRUE(maybe.first.hasValue());

  {
    auto dump = maybe.first.value();
    EXPECT_EQ(1, dump.size());
    EXPECT_EQ("test_value1", dump["test_key"].value);
  }
}

/**
 * Merge same key with diff. values using originator ids
 */
TEST_F(
    MultipleStoreFixture,
    dumpAllWithPrefixMultipleAndParse_sameKeysDiffValues2) {
  //
  // Submit three values in three different stores
  //
  evl.runInEventLoop([&]() noexcept {
    thrift::Value value;
    value.version = 1;
    {
      value.value = "test_value1";
      client1->setKey(
          "test_key", fbzmq::util::writeThriftObjStr(value, serializer), 1);
    }
    {
      value.value = "test_value2";
      client2->setKey(
          "test_key", fbzmq::util::writeThriftObjStr(value, serializer), 1);
    }
    {
      value.value = "test_value3";
      client3->setKey(
          "test_key", fbzmq::util::writeThriftObjStr(value, serializer), 1);
    }

    evl.stop();
  });

  evl.run();

  auto maybe = KvStoreClient::dumpAllWithPrefixMultipleAndParse<thrift::Value>(
      context, urls, "test_");

  ASSERT_TRUE(maybe.first.hasValue());

  {
    auto dump = maybe.first.value();
    EXPECT_EQ(1, dump.size());
    EXPECT_EQ("test_value3", dump["test_key"].value);
  }
}

/**
 * Verify add/del/getPeers APIs
 */
TEST(KvStoreClient, PeerApiTest) {
  fbzmq::Context context;
  const std::string nodeId{"test_store"};
  const std::string peerName1{"peer1"};
  const std::string peerName2{"peer2"};
  const std::string peerName3{"peer3"};
  const thrift::PeerSpec peerSpec1{apache::thrift::FRAGILE,
                                   "inproc://fake_pub_url_1",
                                   "inproc://fake_cmd_url_1"};
  const thrift::PeerSpec peerSpec2{apache::thrift::FRAGILE,
                                   "inproc://fake_pub_url_2",
                                   "inproc://fake_cmd_url_2"};
  const thrift::PeerSpec peerSpec3{apache::thrift::FRAGILE,
                                   "inproc://fake_pub_url_3",
                                   "inproc://fake_cmd_url_3"};

  // Initialize and start KvStore with one fake peer
  std::unordered_map<std::string, thrift::PeerSpec> peers;
  peers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName1),
      std::forward_as_tuple(peerSpec1));
  peers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName2),
      std::forward_as_tuple(peerSpec2));
  peers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peerName3),
      std::forward_as_tuple(peerSpec3));

  auto store = std::make_shared<KvStoreWrapper>(
      context,
      nodeId,
      std::chrono::seconds(60) /* db sync interval */,
      std::chrono::seconds(600) /* counter submit interval */,
      peers);
  store->run();

  // Create another ZmqEventLoop instance for looping clients
  fbzmq::ZmqEventLoop evl;

  // Create and initialize kvstore-clients
  auto client = std::make_shared<KvStoreClient>(
      context, &evl, nodeId, store->localCmdUrl, store->localPubUrl);

  // Schedule callback to set keys from client
  evl.runInEventLoop([&]() noexcept {
    // test addPeers
    client->addPeers(peers);
    {
      auto maybePeers = client->getPeers();
      EXPECT_TRUE(maybePeers.hasValue());
      EXPECT_EQ(*maybePeers, peers);
    }

    // test delPeers
    auto toDelPeers = std::vector<std::string>{peerName1, peerName2};
    peers.erase(peerName1);
    peers.erase(peerName2);
    client->delPeers(toDelPeers);
    {
      auto maybePeers = client->getPeers();
      EXPECT_TRUE(maybePeers.hasValue());
      EXPECT_EQ(*maybePeers, peers);
    }

    // test delPeer
    peers.erase(peerName3);
    client->delPeer(peerName3);
    {
      auto maybePeers = client->getPeers();
      EXPECT_TRUE(maybePeers.hasValue());
      EXPECT_EQ(*maybePeers, peers);
    }

    evl.stop();
  });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "ZmqEventLoop main loop starting.";
    evl.run();
    LOG(INFO) << "ZmqEventLoop main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();

  // Verify peers INFO from KvStore
  const auto peersResponse = store->getPeers();
  EXPECT_EQ(0, peersResponse.size());

  // Stop store
  LOG(INFO) << "Stopping store";
  store->stop();
}

TEST(KvStoreClient, PersistKeyTest) {
  fbzmq::Context context;
  const std::string nodeId{"test_store"};

  // Initialize and start KvStore with one fake peer
  std::unordered_map<std::string, thrift::PeerSpec> peers;
  peers.emplace(
      "peer1",
      thrift::PeerSpec(
          apache::thrift::FRAGILE,
          "inproc://fake_pub_url_1",
          "inproc://fake_cmd_url_1"));
  auto store = std::make_shared<KvStoreWrapper>(
      context,
      nodeId,
      std::chrono::seconds(60) /* db sync interval */,
      std::chrono::seconds(600) /* counter submit interval */,
      peers);
  store->run();

  // Create another ZmqEventLoop instance for looping clients
  fbzmq::ZmqEventLoop evl;

  // Create and initialize kvstore-client, with persist key timer
  auto client1 = std::make_shared<KvStoreClient>(
      context, &evl, nodeId, store->localCmdUrl, store->localPubUrl, 1000ms);

  // Schedule callback to set keys from client1 (this will be executed first)
  evl.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    client1->persistKey("test_key3", "test_value3");
  });

  // Schedule callback to get persist key from client1
  evl.scheduleTimeout(std::chrono::milliseconds(2), [&]() noexcept {
    // 1st get key
    auto maybeVal1 = client1->getKey("test_key3");

    ASSERT(maybeVal1.hasValue());
    EXPECT_EQ(1, maybeVal1->version);
    EXPECT_EQ("test_value3", maybeVal1->value);
  });

  // simulate kvstore restart by erasing the test_key3
  // set a TTL of 1ms in the store so that it gets deleted before refresh event
  evl.scheduleTimeout(std::chrono::milliseconds(3), [&]() noexcept {

    thrift::Value keyExpVal{apache::thrift::FRAGILE,
                                 1,
                                 nodeId,
                                 "test_value3",
                                 1,  /* ttl in msec */
                                 500 /* ttl version */,
                                 0 /* hash */};
    store->setKey("test_key3", keyExpVal);
  });

  // check after few ms if key is deleted,
  evl.scheduleTimeout(std::chrono::milliseconds(30), [&]() noexcept {
    auto maybeVal3 = client1->getKey("test_key3");
    ASSERT_FALSE(maybeVal3.hasValue());
  });

  // Schedule after a second, key will be erased and set back in kvstore
  // with persist key check callback
  evl.scheduleTimeout(std::chrono::milliseconds(3000), [&]() noexcept {
    auto maybeVal3 = client1->getKey("test_key3");
    ASSERT(maybeVal3.hasValue());
    EXPECT_EQ(1, maybeVal3->version);
    EXPECT_EQ("test_value3", maybeVal3->value);
    evl.stop();
  });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "ZmqEventLoop main loop starting.";
    evl.run();
    LOG(INFO) << "ZmqEventLoop main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();

  // Stop store
  LOG(INFO) << "Stopping store";
  store->stop();
}

/**
 * Start a store and attach two clients to it. Set some Keys and add/del peers.
 * Verify that changes are visible in KvStore via a separate REQ socket to
 * KvStore. Further key-2 from client-2 should win over key from client-1
 */
TEST(KvStoreClient, ApiTest) {
  fbzmq::Context context;
  const std::string nodeId{"test_store"};

  // Initialize and start KvStore with one fake peer
  std::unordered_map<std::string, thrift::PeerSpec> peers;
  peers.emplace(
      "peer1",
      thrift::PeerSpec(
          apache::thrift::FRAGILE,
          "inproc://fake_pub_url_1",
          "inproc://fake_cmd_url_1"));
  auto store = std::make_shared<KvStoreWrapper>(
      context,
      nodeId,
      std::chrono::seconds(60) /* db sync interval */,
      std::chrono::seconds(600) /* counter submit interval */,
      peers);
  store->run();

  // Create another ZmqEventLoop instance for looping clients
  fbzmq::ZmqEventLoop evl;

  // Create and initialize kvstore-clients
  auto client1 = std::make_shared<KvStoreClient>(
      context, &evl, nodeId, store->localCmdUrl, store->localPubUrl);
  auto client2 = std::make_shared<KvStoreClient>(
      context, &evl, nodeId, store->localCmdUrl, store->localPubUrl);

  // Schedule callback to set keys from client1 (this will be executed first)
  evl.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    client1->persistKey("test_key1", "test_value1");
    client1->setKey("test_key2", "test_value2");
  });

  // Schedule callback to add/del peer via client-1 (will be executed next)
  evl.scheduleTimeout(std::chrono::milliseconds(1), [&]() noexcept {
    std::unordered_map<std::string, thrift::PeerSpec> peerMap;
    peerMap.emplace(
        "peer2",
        thrift::PeerSpec(
            apache::thrift::FRAGILE,
            "inproc://fake_pub_url_2",
            "inproc://fake_cmd_url_2"));
    EXPECT_TRUE(client1->addPeers(peerMap).hasValue());
    EXPECT_TRUE(client1->delPeer("peer1").hasValue());
  });

  // Schedule callback to persist key2 from client2 (this will be executed next)
  evl.scheduleTimeout(std::chrono::milliseconds(2), [&]() noexcept {
    // 1st get key
    auto maybeVal1 = client2->getKey("test_key2");
    ASSERT(maybeVal1.hasValue());
    EXPECT_EQ(1, maybeVal1->version);
    EXPECT_EQ("test_value2", maybeVal1->value);

    // persistKey with new value
    client2->persistKey("test_key2", "test_value2-client2");

    // 2nd getkey
    auto maybeVal2 = client2->getKey("test_key2");
    ASSERT(maybeVal2.hasValue());
    EXPECT_EQ(2, maybeVal2->version);
    EXPECT_EQ("test_value2-client2", maybeVal2->value);

    // get key with non-existing key
    auto maybeVal3 = client2->getKey("test_key3");
    EXPECT_FALSE(maybeVal3);
  });

  evl.scheduleTimeout(std::chrono::milliseconds(3), [&]() noexcept {
    VLOG(1) << "Running timeout for `setKey` test";
    const std::string testKey{"set_test_key"};
    const thrift::Value testValue{
        apache::thrift::FRAGILE,
        3,
        "originator-id",
        "set_test_value",
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        generateHash(
            3,
            "originator-id",
            folly::Optional<std::string>("set_test_value")) /* hash */};

    // Sync call to insert key-value into the KvStore
    client1->setKey(testKey, testValue);

    // Sync call to get key-value from KvStore
    auto maybeValue = store->getKey(testKey);
    ASSERT(maybeValue);
    EXPECT_EQ(testValue, *maybeValue);
  });

  // dump keys
  evl.scheduleTimeout(std::chrono::milliseconds(4), [&]() noexcept {
    // dump keys
    const auto maybeKeyVals = client1->dumpAllWithPrefix();
    ASSERT(maybeKeyVals.hasValue());
    ASSERT_EQ(3, maybeKeyVals->size());
    EXPECT_EQ("test_value1", maybeKeyVals->at("test_key1").value);
    EXPECT_EQ("test_value2-client2", maybeKeyVals->at("test_key2").value);
    EXPECT_EQ("set_test_value", maybeKeyVals->at("set_test_key").value);

    const auto maybeKeyVals2 = client2->dumpAllWithPrefix();
    ASSERT(maybeKeyVals2.hasValue());
    EXPECT_EQ(*maybeKeyVals, *maybeKeyVals2);

    // dump keys with a given prefix
    const auto maybePrefixedKeyVals = client1->dumpAllWithPrefix("test");
    ASSERT(maybePrefixedKeyVals.hasValue());
    ASSERT_EQ(2, maybePrefixedKeyVals->size());
    EXPECT_EQ("test_value1", maybePrefixedKeyVals->at("test_key1").value);
    EXPECT_EQ(
        "test_value2-client2", maybePrefixedKeyVals->at("test_key2").value);
  });

  // Inject keys w/ TTL
  evl.scheduleTimeout(std::chrono::milliseconds(5), [&]() noexcept {
    const thrift::Value testValue1{apache::thrift::FRAGILE,
                                   1,
                                   nodeId,
                                   "test_ttl_value1",
                                   kTtl.count(),
                                   500 /* ttl version */,
                                   0 /* hash */};
    client1->setKey("test_ttl_key1", testValue1);
    client1->persistKey("test_ttl_key1", "test_ttl_value1", kTtl);

    client2->setKey("test_ttl_key2", "test_ttl_value2", 1, kTtl);
    const thrift::Value testValue2{apache::thrift::FRAGILE,
                                   1,
                                   nodeId,
                                   "test_ttl_value2",
                                   kTtl.count(),
                                   1500 /* ttl version */,
                                   0 /* hash */};
    client2->setKey("test_ttl_key2", testValue2);
  });

  // Keys shall not expire even after TTL bcoz client is updating their TTL
  evl.scheduleTimeout(std::chrono::milliseconds(6) + kTtl * 3, [&]() noexcept {
    LOG(INFO) << "received response.";
    auto maybeVal1 = client2->getKey("test_ttl_key1");
    ASSERT(maybeVal1.hasValue());
    EXPECT_EQ("test_ttl_value1", maybeVal1->value);
    EXPECT_LT(500, maybeVal1->ttlVersion);

    auto maybeVal2 = client1->getKey("test_ttl_key2");
    ASSERT(maybeVal2.hasValue());
    EXPECT_LT(1500, maybeVal2->ttlVersion);
    EXPECT_EQ(1, maybeVal2->version);
    EXPECT_EQ("test_ttl_value2", maybeVal2->value);

    // stop the event loop
    evl.stop();
  });

  // Start the event loop and wait until it is finished execution.
  std::thread evlThread([&]() {
    LOG(INFO) << "ZmqEventLoop main loop starting.";
    evl.run();
    LOG(INFO) << "ZmqEventLoop main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();

  /* sleep override */
  // wait until TTL keys expire; clients stop updating TTL
  std::this_thread::sleep_for(kTtl * 3);

  // Verify peers INFO from KvStore
  const auto peersResponse = store->getPeers();
  EXPECT_EQ(1, peersResponse.size());
  EXPECT_EQ(0, peersResponse.count("peer1"));
  EXPECT_EQ(1, peersResponse.count("peer2"));

  // Verify key-value info
  const auto keyValResponse = store->dumpAll();
  LOG(INFO) << "received response.";
  for (const auto& kv : keyValResponse) {
    VLOG(4) << "key: " << kv.first << ", val: " << kv.second.value.value();
  }
  ASSERT_EQ(3, keyValResponse.size());

  auto const& value1 = keyValResponse.at("test_key1");
  EXPECT_EQ("test_value1", value1.value);
  EXPECT_EQ(1, value1.version);

  auto const& value2 = keyValResponse.at("test_key2");
  EXPECT_EQ("test_value2-client2", value2.value);
  EXPECT_LE(2, value2.version); // client-2 must win over client-1

  EXPECT_EQ(1, keyValResponse.count("set_test_key"));

  // Stop store
  LOG(INFO) << "Stopping store";
  store->stop();
}

TEST(KvStoreClient, SubscribeApiTest) {
  fbzmq::Context context;
  const std::string nodeId{"test_store"};

  // Initialize and start KvStore with empty peer
  const std::unordered_map<std::string, thrift::PeerSpec> emptyPeers;
  auto store = std::make_shared<KvStoreWrapper>(
      context,
      nodeId,
      std::chrono::seconds(60) /* db sync interval */,
      std::chrono::seconds(3600) /* counter submit interval */,
      emptyPeers);
  store->run();

  // Create another ZmqEventLoop instance for looping clients
  fbzmq::ZmqEventLoop evl;

  // Create and initialize kvstore-clients
  auto client1 = std::make_shared<KvStoreClient>(
      context, &evl, nodeId, store->localCmdUrl, store->localPubUrl);
  auto client2 = std::make_shared<KvStoreClient>(
      context, &evl, nodeId, store->localCmdUrl, store->localPubUrl);

  int key1CbCnt = 0;
  int key2CbCnt = 0;
  // Schedule callback to set keys from client1 (this will be executed first)
  evl.scheduleTimeout(std::chrono::milliseconds(0), [&]() noexcept {
    client1->subscribeKey(
        "test_key1", [&](std::string const& k,
            folly::Optional<thrift::Value>  v) {
          // this should be called when client1 call persistKey for test_key1
          EXPECT_EQ("test_key1", k);
          EXPECT_EQ(1, v.value().version);
          EXPECT_EQ("test_value1", v.value().value);
          key1CbCnt++;
        }, false);
    client1->subscribeKey(
        "test_key2", [&](std::string const& k,
            folly::Optional<thrift::Value> v) {
          // this should be called when client2 call persistKey for test_key2
          EXPECT_EQ("test_key2", k);
          EXPECT_LT(0, v.value().version);
          EXPECT_GE(2, v.value().version);
          switch (v.value().version) {
          case 1:
            EXPECT_EQ("test_value2", v.value().value);
            break;
          case 2:
            EXPECT_EQ("test_value2-client2", v.value().value);
            break;
          }
          key2CbCnt++;
        }, false);
    client1->persistKey("test_key1", "test_value1");
    client1->setKey("test_key2", "test_value2");
  });

  int key2CbCntClient2{0};

  // Schedule callback to persist key2 from client2 (this will be executed next)
  evl.scheduleTimeout(std::chrono::milliseconds(10), [&]() noexcept {
    client2->persistKey("test_key2", "test_value2-client2");
    client2->subscribeKey(
        "test_key2",
        [&](std::string const& /* k */,
            folly::Optional<thrift::Value> /* v */) {
          // this should never be called when client2 call persistKey
          // for test_key2 with same value
          key2CbCntClient2++;
        }, false);
    // call persistkey with same value. should not get a callback here.
    client2->persistKey("test_key2", "test_value2-client2");
  });

  /* test for key callback with the option of getting key Value */
  int keyExpKeySubCbCnt{0}; /* reply count for key regd. with fetchValue=true */
  evl.scheduleTimeout(std::chrono::milliseconds(11), [&]() noexcept {

    client2->setKey("test_key_subs_cb", "test_key_subs_cb_val", 11);

    folly::Optional<thrift::Value> keyValue;
    /* register key callback with the option of getting key Value */
    keyValue = client2->subscribeKey(
      "test_key_subs_cb",
      [&](std::string const& /* unused */,
          folly::Optional<thrift::Value> /* v */) {
      }, true);

    if (keyValue.hasValue()) {
      EXPECT_EQ("test_key_subs_cb_val", keyValue.value().value);
      keyExpKeySubCbCnt++;
    }
  });

  /* test for expired keys update */
  int keyExpKeyCbCnt{0};  /* expired key call back count specific to a key */
  int keyExpCbCnt{0};  /* expired key call back count */
  evl.scheduleTimeout(std::chrono::milliseconds(20), [&]() noexcept {

    thrift::Value keyExpVal{apache::thrift::FRAGILE,
                                   1,
                                   nodeId,
                                   "test_key_exp_val",
                                   1,  /* ttl in msec */
                                   500 /* ttl version */,
                                   0 /* hash */};

    /* register client callback for key updates from KvStore */
    client2->setKvCallback(
        [&](const std::string& key,
        folly::Optional<thrift::Value> thriftVal) noexcept {
      if (!thriftVal.hasValue()) {
        EXPECT_EQ("test_key_exp", key);
        keyExpCbCnt++;
      }
    });

    /* register key callback for key updates from KvStore */
    client2->subscribeKey(
        "test_key_exp",
        [&](std::string const& k, folly::Optional<thrift::Value> v) {
          if (!v.hasValue()) {
            EXPECT_EQ("test_key_exp", k);
            keyExpKeyCbCnt++;
            evl.stop();
          }
    }, false);

    store->setKey("test_key_exp", keyExpVal);
  });

  // Schedule timeout for terminating the event loop
  evl.scheduleTimeout(
      std::chrono::milliseconds(60), [&]() noexcept { evl.stop(); });

  // Start the event loop
  std::thread evlThread([&]() {
    LOG(INFO) << "ZmqEventLoop main loop starting.";
    evl.run();
    LOG(INFO) << "ZmqEventLoop main loop terminating.";
  });
  evl.waitUntilRunning();
  evl.waitUntilStopped();
  evlThread.join();

  // Verify out expectations
  EXPECT_EQ(1, key1CbCnt);
  EXPECT_GE(2, key2CbCnt); // This can happen when KvStore processes request
  EXPECT_LE(1, key2CbCnt); // from two clients in out of order. However values
                           // are going to be same.
  EXPECT_EQ(0, key2CbCntClient2);
  EXPECT_EQ(1, keyExpCbCnt);
  EXPECT_EQ(1, keyExpKeyCbCnt);
  EXPECT_EQ(1, keyExpKeySubCbCnt);

  // Stop server
  LOG(INFO) << "Stopping store";
  store->stop();
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // init sodium security library
  if (::sodium_init() == -1) {
    LOG(ERROR) << "Failed initializing sodium";
    return 1;
  }

  // Run the tests
  return RUN_ALL_TESTS();
}
