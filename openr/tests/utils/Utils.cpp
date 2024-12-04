/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/utils/Utils.h>
#include <chrono>
#include <thread>

namespace detail {
// Prefix length of a subnet
const uint8_t kBitMaskLen = 128;
const size_t kMaxConcurrency = 10;
} // namespace detail

namespace openr {

/*
 * Util function to generate random string of given length
 */
std::string
genRandomStr(const int64_t len) {
  std::string s;
  s.resize(len);

  static const std::string alphanum =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  for (int64_t i = 0; i < len; ++i) {
    s[i] = alphanum[folly::Random::rand32() % alphanum.size()];
  }
  return s;
}

/*
 * Util function to generate random string of given length with specified prefix
 */
std::string
genRandomStrWithPrefix(const std::string& prefix, const unsigned long len) {
  std::string s;
  s.resize(std::max(len, prefix.size()));

  static const std::string alphanum =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

  for (int64_t i = 0; i < prefix.size(); ++i) {
    s[i] = prefix.at(i);
  }

  for (int64_t i = prefix.size(); i < len; ++i) {
    s[i] = alphanum[folly::Random::rand32() % alphanum.size()];
  }
  return s;
}

/*
 * Util function to construct thrift::AreaConfig
 */
openr::thrift::AreaConfig
createAreaConfig(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes,
    const std::optional<std::string>& policy) {
  openr::thrift::AreaConfig areaConfig;
  areaConfig.area_id() = areaId;
  areaConfig.neighbor_regexes() = neighborRegexes;
  areaConfig.include_interface_regexes() = interfaceRegexes;
  if (policy) {
    areaConfig.import_policy_name() = policy.value();
  }

  return areaConfig;
}

/*
 * Util function to genearate basic Open/R config in test environment:
 *  1) Unit Test;
 *  2) Benchmark Test;
 *  3) TBD;
 */
openr::thrift::OpenrConfig
getBasicOpenrConfig(
    const std::string& nodeName,
    const std::vector<openr::thrift::AreaConfig>& areaCfg,
    bool enableV4,
    bool dryrun,
    bool enableV4OverV6Nexthop) {
  /*
   * [DEFAULT] thrift::OpenrConfig
   */
  openr::thrift::OpenrConfig config;

  /*
   * [OVERRIDE] config knob toggling
   */
  config.node_name() = nodeName;
  config.enable_v4() = enableV4;
  config.v4_over_v6_nexthop() = enableV4OverV6Nexthop;
  config.dryrun() = dryrun;
  config.ip_tos() = 192;

  config.enable_rib_policy() = true;
  config.assume_drained() = false;
  config.enable_neighbor_monitor() = true;
  config.enable_init_optimization() = true;

  /*
   * [OVERRIDE] thrift::LinkMonitorConfig
   */
  openr::thrift::LinkMonitorConfig lmConf;
  lmConf.enable_perf_measurement() = false;
  lmConf.use_rtt_metric() = true;
  config.link_monitor_config() = lmConf;

  /*
   * [OVERRIDE] thrift::KvStoreConfig
   */
  openr::thrift::KvstoreConfig kvstoreConfig;
  config.kvstore_config() = kvstoreConfig;

  /*
   * [OVERRIDE] thrift::SparkConfig
   */
  openr::thrift::SparkConfig sparkConfig;
  sparkConfig.hello_time_s() = 2;
  sparkConfig.keepalive_time_s() = 1;
  sparkConfig.fastinit_hello_time_ms() = 100;
  sparkConfig.hold_time_s() = 2;
  sparkConfig.graceful_restart_time_s() = 6;
  sparkConfig.min_neighbor_discovery_interval_s() = 2;
  sparkConfig.max_neighbor_discovery_interval_s() = 4;
  config.spark_config() = sparkConfig;

  /*
   * [OVERRIDE] thrift::AreaConfig
   */
  if (areaCfg.empty()) {
    config.areas() = {
        createAreaConfig(kTestingAreaName, {".*"}, {".*"}, std::nullopt)};
  } else {
    config.areas() = areaCfg;
  }

  return config;
}

std::vector<thrift::PrefixEntry>
generatePrefixEntries(const PrefixGenerator& prefixGenerator, uint32_t num) {
  // generate `num` of random prefixes
  std::vector<thrift::IpPrefix> prefixes =
      prefixGenerator.ipv6PrefixGenerator(num, ::detail::kBitMaskLen);
  auto tPrefixEntries =
      folly::gen::from(prefixes) |
      folly::gen::mapped([](const thrift::IpPrefix& prefix) {
        return createPrefixEntry(prefix, thrift::PrefixType::DEFAULT);
      }) |
      folly::gen::as<std::vector<thrift::PrefixEntry>>();
  return tPrefixEntries;
}

DecisionRouteUpdate
generateDecisionRouteUpdateFromPrefixEntries(
    std::vector<thrift::PrefixEntry> prefixEntries, uint32_t areaId) {
  // Borrow the settings for prefixEntries from PrefixManagerTest
  auto path1 =
      createNextHop(toBinaryAddress(folly::IPAddress("fe80::2")), "iface", 1);
  path1.area() = std::to_string(areaId);
  DecisionRouteUpdate routeUpdate;

  for (auto& prefixEntry : prefixEntries) {
    prefixEntry.area_stack() = {"65000"};
    prefixEntry.metrics()->distance() = 1;
    prefixEntry.type() = thrift::PrefixType::DEFAULT;
    prefixEntry.forwardingAlgorithm() =
        thrift::PrefixForwardingAlgorithm::SP_ECMP;
    prefixEntry.forwardingType() = thrift::PrefixForwardingType::IP;
    prefixEntry.minNexthop() = 10;

    auto unicastRoute = RibUnicastEntry(
        toIPNetwork(*prefixEntry.prefix()),
        {path1},
        prefixEntry,
        std::to_string(areaId),
        false);
    routeUpdate.addRouteToUpdate(unicastRoute);
  }
  return routeUpdate;
}

DecisionRouteUpdate
generateDecisionRouteUpdate(
    const PrefixGenerator& prefixGenerator, uint32_t num, uint32_t areaId) {
  std::vector<thrift::PrefixEntry> prefixEntries =
      generatePrefixEntries(prefixGenerator, num);
  return generateDecisionRouteUpdateFromPrefixEntries(prefixEntries, areaId);
}

/*
 * Util function to generate kvstore keyVal
 */
std::pair<std::string, thrift::Value>
genRandomKvStoreKeyVal(
    int64_t keyLen,
    int64_t valLen,
    int64_t version,
    const std::string& originatorId,
    int64_t ttl,
    int64_t ttlVersion,
    std::optional<int64_t> hash) {
  auto key = genRandomStr(keyLen);
  auto value = genRandomStr(valLen);
  auto thriftVal = createThriftValue(
      version /* version */,
      originatorId /* originatorId */,
      value /* value */,
      ttl /* ttl */,
      ttlVersion /* ttl version */,
      hash /* hash */);

  return std::make_pair(key, thriftVal);
}

/*
 * Util function to trigger initialization event for PrefixManager
 */
void
triggerInitializationEventForPrefixManager(
    messaging::ReplicateQueue<DecisionRouteUpdate>& fibRouteUpdatesQ,
    messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQ) {
  // condition 1: publish update for thrift::PrefixType::RIB
  DecisionRouteUpdate fullSyncUpdates;
  fullSyncUpdates.type = DecisionRouteUpdate::FULL_SYNC;
  fibRouteUpdatesQ.push(std::move(fullSyncUpdates));

  // condition 2: publish KVSTORE_SYNCED and ADJACENCY_DB_SYNCED signal
  kvStoreUpdatesQ.push(thrift::InitializationEvent::KVSTORE_SYNCED);
  kvStoreUpdatesQ.push(thrift::InitializationEvent::ADJACENCY_DB_SYNCED);
}

/*
 * Util function to trigger initialization event for PrefixManager
 */
void
triggerInitializationEventKvStoreSynced(
    messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQ) {
  kvStoreUpdatesQ.push(thrift::InitializationEvent::KVSTORE_SYNCED);
}

/*
 * Util function to generate Adjacency Value
 */
thrift::Value
createAdjValue(
    apache::thrift::CompactSerializer serializer,
    const std::string& node,
    int64_t version,
    const std::vector<thrift::Adjacency>& adjs,
    bool overloaded,
    int32_t nodeId,
    int64_t nodeMetricIncrement) {
  auto adjDB = createAdjDb(node, adjs, nodeId);
  adjDB.isOverloaded() = overloaded;
  adjDB.nodeMetricIncrementVal() = nodeMetricIncrement;
  return createThriftValue(
      version,
      "originator-1",
      writeThriftObjStr(adjDB, serializer),
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
}

/*
 * Util function to generate Adjacency Value with linkstatus
 */
thrift::Value
createAdjValueWithLinkStatus(
    apache::thrift::CompactSerializer serializer,
    const std::string& node,
    int64_t version,
    const std::vector<thrift::Adjacency>& adjs,
    thrift::LinkStatusRecords rec,
    bool overloaded,
    int32_t nodeId) {
  auto adjDB = createAdjDb(node, adjs, nodeId);
  adjDB.isOverloaded() = overloaded;
  adjDB.linkStatusRecords() = rec;
  return createThriftValue(
      version,
      "originator-1",
      writeThriftObjStr(adjDB, serializer),
      Constants::kTtlInfinity /* ttl */,
      0 /* ttl version */,
      0 /* hash */);
}

/*
 * Util function to check if two publications are equal without checking
 * equality of hash and nodeIds
 */
bool
equalPublication(thrift::Publication&& pub1, thrift::Publication&& pub2) {
  if ((*pub1.keyVals()).size() != (*pub2.keyVals()).size()) {
    return false;
  }

  // make ordered copies of KeyVals
  std::map<std::string, thrift::Value> pub1KeyVals(
      (*pub1.keyVals()).begin(), (*pub1.keyVals()).end());
  std::map<std::string, thrift::Value> pub2KeyVals(
      (*pub2.keyVals()).begin(), (*pub2.keyVals()).end());

  for (auto it1 = pub1KeyVals.begin(), it2 = pub2KeyVals.begin();
       it1 != pub1KeyVals.end() and it2 != pub2KeyVals.end();
       ++it1, ++it2) {
    // check the key matches
    if (it1->first != it2->first) {
      return false;
    }

    if (compareValues(it1->second, it2->second) != ComparisonResult::TIED) {
      return false;
    }
  }

  if (pub1.area() != pub2.area()) {
    return false;
  }

  if (pub1.timestamp_ms() != pub2.timestamp_ms()) {
    return false;
  }

  std::sort((*pub1.expiredKeys()).begin(), (*pub1.expiredKeys()).end());
  std::sort((*pub2.expiredKeys()).begin(), (*pub2.expiredKeys()).end());

  if (pub1.expiredKeys() != pub2.expiredKeys()) {
    return false;
  }

  if (pub1.tobeUpdatedKeys().has_value() and
      pub2.tobeUpdatedKeys().has_value()) {
    std::sort(
        (*pub1.tobeUpdatedKeys()).begin(), (*pub1.tobeUpdatedKeys()).end());
    std::sort(
        (*pub2.tobeUpdatedKeys()).begin(), (*pub2.tobeUpdatedKeys()).end());
  }

  if (pub1.tobeUpdatedKeys() != pub2.tobeUpdatedKeys()) {
    return false;
  }

  return true;
}

std::string
genNodeName(size_t i) {
  return folly::to<std::string>("node-", i);
}

void
generateTopo(
    const std::vector<std::unique_ptr<KvStoreWrapper<
        apache::thrift::Client<thrift::KvStoreService>>>>& stores,
    ClusterTopology topo) {
  switch (topo) {
    /*
     * Linear Topology Illustration:
     * 0 - 1 - 2 - 3 - 4 - 5 - 6 - 7
     */
  case ClusterTopology::LINEAR: {
    if (stores.empty()) {
      // no peers to connect
      return;
    }
    KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* prev =
        stores.front().get();
    for (size_t i = 1; i < stores.size(); i++) {
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* cur =
          stores.at(i).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
      prev = cur;
    }
    break;
  }
  /*
   * Ring Topology Illustration:
   *   1 - 3 - 5
   *  /         \
   * 0           7
   *  \         /
   *   2 - 4 - 6
   * This is designed such that the last node is the furthest from first node
   */
  case ClusterTopology::RING: {
    for (size_t i = 1; i < stores.size(); i++) {
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* cur =
          stores.at(i).get();
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* prev =
          stores.at(i == 1 ? 0 : i - 2).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
    }
    if (stores.size() > 2) {
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* cur =
          stores.back().get();
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* prev =
          stores.at(stores.size() - 2).get();
      prev->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(kTestingAreaName, prev->getNodeId(), prev->getPeerSpec());
    }
    break;
  }
  /*
   * Star Topology Illustration:
   *    1   2
   *     \ /
   *  6 - 0 - 3
   *     / \
   *    5   4
   * Every additional node is directly connected to center
   */
  case ClusterTopology::STAR: {
    for (size_t i = 1; i < stores.size(); i++) {
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* center =
          stores.front().get();
      KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>* cur =
          stores.at(i).get();
      center->addPeer(kTestingAreaName, cur->getNodeId(), cur->getPeerSpec());
      cur->addPeer(
          kTestingAreaName, center->getNodeId(), center->getPeerSpec());
    }
    break;
  }
  default: {
    throw std::runtime_error("invalid topology type");
  }
  }
}

#if FOLLY_HAS_COROUTINES
folly::coro::Task<void>
co_validateNodeKey(
    const std::unordered_map<std::string, ::openr::thrift::Value>& events,
    ::openr::KvStoreWrapper<apache::thrift::Client<thrift::KvStoreService>>*
        node,
    int timeoutSec) {
  auto startTime = std::chrono::steady_clock::now();
  while (events != node->dumpAll(kTestingAreaName)) {
    auto curWaitTime = std::chrono::steady_clock::now() - startTime;
    if (curWaitTime > std::chrono::seconds(timeoutSec)) {
      XLOG(FATAL) << fmt::format(
          "exceed wait time {} seconds. expected size: {}, actual size: {} for node {}",
          timeoutSec,
          events.size(),
          node->dumpAll(kTestingAreaName).size(),
          node->getNodeId());
    }
    // yield to avoid hogging the process
    std::this_thread::yield();
  }
  co_return;
}

folly::coro::Task<void>
co_waitForConvergence(
    const std::unordered_map<std::string, ::openr::thrift::Value>& events,
    const std::vector<std::unique_ptr<::openr::KvStoreWrapper<
        apache::thrift::Client<thrift::KvStoreService>>>>& stores) {
  co_await folly::coro::collectAllWindowed(
      [&]() -> folly::coro::Generator<folly::coro::Task<void>&&> {
        for (size_t i = 0; i < stores.size(); i++) {
          co_yield co_validateNodeKey(events, stores.at(i).get());
        }
      }(),
      ::detail::kMaxConcurrency);
}
#endif

} // namespace openr
