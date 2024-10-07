/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>

#include <folly/Conv.h>
#include <folly/gen/Base.h>
#include <folly/init/Init.h>

#if FOLLY_HAS_COROUTINES
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Generator.h>
#include <folly/coro/Task.h>
#endif

#include <openr/common/Constants.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/Types.h>
#include <openr/common/Util.h>
#include <openr/config/Config.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreWrapper.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/tests/mocks/PrefixGenerator.h>

namespace openr {

// The byte size of a key
const size_t kSizeOfKey = 32;
// The byte size of a value
const size_t kSizeOfValue = 1024;

enum class OperationType {
  ADD_NEW_KEY = 0,
  UPDATE_VERSION = 1,
  UPDATE_TTL = 2,
};

/*
 * Util function to generate random string of given length
 */
std::string genRandomStr(const int64_t len);

/*
 * Util function to generate random string of given length with specified prefix
 */
std::string genRandomStrWithPrefix(
    const std::string& prefix, const unsigned long len);

/*
 * Util function to construct thrift::AreaConfig
 */
openr::thrift::AreaConfig createAreaConfig(
    const std::string& areaId,
    const std::vector<std::string>& neighborRegexes,
    const std::vector<std::string>& interfaceRegexes,
    const std::optional<std::string>& policy = std::nullopt);

/*
 * Util function to genearate basic Open/R config in UT env.
 */
openr::thrift::OpenrConfig getBasicOpenrConfig(
    const std::string& nodeName = "",
    const std::vector<openr::thrift::AreaConfig>& areaCfg = {},
    bool enableV4 = true,
    bool enableSegmentRouting = false,
    bool dryrun = true,
    bool enableV4OverV6Nexthop = false);

std::vector<thrift::PrefixEntry> generatePrefixEntries(
    const PrefixGenerator& prefixGenerator, uint32_t num);

DecisionRouteUpdate generateDecisionRouteUpdateFromPrefixEntries(
    std::vector<thrift::PrefixEntry> prefixEntries, uint32_t areaId = 0);

DecisionRouteUpdate generateDecisionRouteUpdate(
    const PrefixGenerator& prefixGenerator, uint32_t num, uint32_t areaId = 0);

std::pair<std::string, thrift::Value> genRandomKvStoreKeyVal(
    int64_t keyLen,
    int64_t valLen,
    int64_t version,
    const std::string& originatorId = "originator",
    int64_t ttl = Constants::kTtlInfinity,
    int64_t ttlVersion = 0,
    std::optional<int64_t> hash = std::nullopt);

/*
 * Util function to trigger initialization event for PrefixManager
 */
void triggerInitializationEventForPrefixManager(
    messaging::ReplicateQueue<DecisionRouteUpdate>& fibRouteUpdatesQ,
    messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQ);

/*
 * Util function to trigger initialization event KVSTORE_SYNCED
 */
void triggerInitializationEventKvStoreSynced(
    messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQ);

/*
 * Util function to generate Adjacency Value
 */
thrift::Value createAdjValue(
    apache::thrift::CompactSerializer serializer,
    const std::string& node,
    int64_t version,
    const std::vector<thrift::Adjacency>& adjs,
    bool overloaded = false,
    int32_t nodeId = 0,
    int64_t nodeMetricIncrement = 0);

/*
 * Util function to generate Adjacency Value with LinkStatus record
 */
thrift::Value createAdjValueWithLinkStatus(
    apache::thrift::CompactSerializer serializer,
    const std::string& node,
    int64_t version,
    const std::vector<thrift::Adjacency>& adjs,
    thrift::LinkStatusRecords rec,
    bool overloaded = false,
    int32_t nodeId = 0);

/*
 * Util function to check if two publications are equal without checking
 * equality of hash and nodeIds
 */
bool equalPublication(thrift::Publication&& pub1, thrift::Publication&& pub2);

/*
 * Util function to generate unique node name based on index `i`
 */
std::string genNodeName(size_t i);

enum class ClusterTopology {
  LINEAR = 0,
  RING = 1,
  STAR = 2,
  // TODO: add more topo
};

/*
 * Util function to generate kvstore topology
 */
void generateTopo(
    const std::vector<std::unique_ptr<::openr::KvStoreWrapper<
        apache::thrift::Client<::openr::thrift::KvStoreService>>>>& stores,
    ClusterTopology topo);

struct CheckTimeoutException : public std::exception {
 public:
  explicit CheckTimeoutException(const std::string& msg) : msg_(msg) {}

  inline const char*
  what() const noexcept override {
    return msg_.c_str();
  }

 private:
  const std::string msg_;
};

/*
 * helper function to check until timed out
 * checkFn: check funtion that returns a boolean (true upon success)
 * timeout (e.g 1000ms), retry indefinitely if set to 0
 * retryInterval (e.g 100ms)
 */
template <typename CHECK_FN>
void
checkUntilTimeout(
    CHECK_FN checkFn,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
    std::chrono::milliseconds retryInterval = std::chrono::milliseconds(100)) {
  bool checkIndefinitely = timeout.count() == 0;
  if (not checkIndefinitely) {
    CHECK_GE(timeout.count(), retryInterval.count());
  }

  auto maxTimePoint = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (checkFn()) {
      return;
    }
    bool isTimedOut =
        !checkIndefinitely && std::chrono::steady_clock::now() > maxTimePoint;
    if (isTimedOut) {
      throw CheckTimeoutException(
          fmt::format("timed out at {} ms!", timeout.count()));
    }
    /* sleep override */
    std::this_thread::sleep_for(retryInterval);
  }
}

#if FOLLY_HAS_COROUTINES
/*
 * Util function to validate if the given node has received all events
 */
folly::coro::Task<void> co_validateNodeKey(
    const std::unordered_map<std::string, ::openr::thrift::Value>& events,
    ::openr::KvStoreWrapper<
        apache::thrift::Client<::openr::thrift::KvStoreService>>* node,
    int timeoutSec = 30);

/*
 * Util function to validate if all nodes have received all events
 */
folly::coro::Task<void> co_waitForConvergence(
    const std::unordered_map<std::string, ::openr::thrift::Value>& events,
    const std::vector<std::unique_ptr<::openr::KvStoreWrapper<
        apache::thrift::Client<::openr::thrift::KvStoreService>>>>& stores);
#endif
} // namespace openr
