/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncSocket.h>
#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

#include <folly/ssl/SSLSessionManager.h>

namespace openr {

enum class MergeType {
  UPDATE_ALL_NEEDED = 0,
  UPDATE_TTL_NEEDED = 1,
  RESYNC_NEEDED = 2,
  NO_UPDATE_NEEDED = 3,
};

/*
 * [FSM] KvStore peer event ENUM which triggers the peer state transition
 */
enum class KvStorePeerEvent {
  PEER_ADD = 0,
  PEER_DEL = 1,
  SYNC_RESP_RCVD = 2,
  THRIFT_API_ERROR = 3,
  INCONSISTENCY_DETECTED = 4,
};

/*
 * [Self Originated Key Management]
 *
 * This is the structure wrapper containing the:
 *  1) self-originated value;
 *  2) key backoff;
 *  3) ttl backoffs;
 */
struct SelfOriginatedValue {
  // Value associated with the self-originated key
  thrift::Value value;
  // Backoff for advertising key-val to kvstore_. Only for persisted key-vals.
  std::optional<ExponentialBackoff<std::chrono::milliseconds>> keyBackoff;
  // Backoff for advertising ttl updates for this key-val
  ExponentialBackoff<std::chrono::milliseconds> ttlBackoff;

  SelfOriginatedValue() = default;
  explicit SelfOriginatedValue(const thrift::Value& val) : value(val) {}
};

using SelfOriginatedKeyVals =
    std::unordered_map<std::string, SelfOriginatedValue>;

class KvStoreFilters {
 public:
  // takes the list of comma separated key prefixes to match,
  // and the list of originator IDs to match in the value
  explicit KvStoreFilters(
      std::vector<std::string> const& keyPrefix,
      std::set<std::string> const& originatorIds,
      thrift::FilterOperator const& filterOperator =
          thrift::FilterOperator::OR);

  // Check if key matches the filters
  bool keyMatchAny(std::string const& key, thrift::Value const& value) const;

  // Check if key matches all the filters
  bool keyMatchAll(std::string const& key, thrift::Value const& value) const;

  bool keyMatch(std::string const& key, thrift::Value const& value) const;

  // overload the function for key only match
  bool keyMatch(std::string const& key) const;

  // return comma separeated string prefix
  std::vector<std::string> getKeyPrefixes() const;

  // return set of origninator IDs
  std::set<std::string> getOriginatorIdList() const;

  // print filters
  std::string str() const;

 private:
  // list of string prefixes, empty list matches all keys
  std::vector<std::string> keyPrefixList_{};

  // set of node IDs to match, empty set matches all nodes
  std::set<std::string> originatorIds_{};

  // keyPrefix class to create RE2 set and to match keys
  RegexSet keyRegexSet_;

  // filter's OR/AND matching logic for attributes
  thrift::FilterOperator filterOperator_;
};

// helper for deserialization
template <typename ThriftType>
static ThriftType parseThriftValue(thrift::Value const& value);

/**
 * Given map of thrift::Value object parse them into map of ThriftType
 * objects,
 * while retaining the versioning information
 */
template <typename ThriftType>
static std::unordered_map<std::string, ThriftType> parseThriftValues(
    const thrift::KeyVals& keyVals);

/**
 * Similar to the above but parses the values according to the ThriftType
 * passed. This will hide the version/originator & other details
 *
 * @template param ThriftType - decode values as this thrift type.
 *  This is handy when you dump keys with the same prefix (which we do)
 *
 * @param sockAddrs - (address, port) to connect OpenR instance to
 * @param prefix - the key prefix used for key dumping. Dump all if empty
 * @param connectTimeout - timeout value set on connecting server
 * @param processTimeout - timeout value set on porcessing request
 * @param sslContext - context to use for SSL connection
 * @param maybeIpTos - IP_TOS value for control plane if passed in
 * @param bindAddr - source addr for binding purpose. Default will be ANY
 *
 * @return
 *  - First member of the pair is key-value map obtained by merging data
 *    from all stores. Null value if failed connecting and obtaining snapshot
 *    from ALL stores. If at least one store responds this will be non-empty.
 *  - Second member of the pair is a list of unreachable addresses
 */
template <typename ThriftType, typename ClientType>
static std::pair<
    std::optional<std::unordered_map<std::string /* key */, ThriftType>>,
    std::vector<folly::SocketAddress> /* unreachable url */>
dumpAllWithPrefixMultipleAndParse(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& prefix,
    std::chrono::milliseconds connectTimeout = Constants::kServiceConnTimeout,
    std::chrono::milliseconds processTimeout = Constants::kServiceProcTimeout,
    const std::shared_ptr<folly::SSLContext> sslContext = nullptr,
    std::optional<int> maybeIpTos = std::nullopt,
    const folly::SocketAddress& bindAddr = folly::AsyncSocket::anyAddress());

template <typename ThriftType, typename ClientType>
static std::unordered_map<std::string /* key */, ThriftType>
dumpAllWithPrefixMultipleAndParse(
    const AreaId& area,
    const std::vector<std::unique_ptr<ClientType>>& clients,
    const std::string& prefix);

/*
 * This will be a static method to do a full-dump of KvStore key-val to
 * multiple KvStore instances. It will fetch values from different KvStore
 * instances and merge them together to finally return thrift::Value
 *
 * @param sockAddrs - (address, port) to connect OpenR instance to
 * @param prefix - the key prefix used for key dumping. Dump all if empty
 * @param connectTimeout - timeout value set on connecting server
 * @param processTimeout - timeout value set on porcessing request
 * @param sslContext - context to use for SSL connection
 * @param maybeIpTos - IP_TOS value for control plane if passed in
 * @param bindAddr - source addr for binding purpose. Default will be ANY
 *
 * @return
 *  - First member of the pair is key-value map obtained by merging data
 *    from all stores. Null value if failed connecting and obtaining snapshot
 *    from ALL stores. If at least one store responds this will be non-empty.
 *  - Second member of the pair is a list of unreachable addresses
 */
template <typename ClientType>
static std::pair<
    std::optional<thrift::KeyVals>,
    std::vector<folly::SocketAddress> /* unreachable addresses */>
dumpAllWithThriftClientFromMultiple(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& prefix,
    std::chrono::milliseconds connectTimeout = Constants::kServiceConnTimeout,
    std::chrono::milliseconds processTimeout = Constants::kServiceProcTimeout,
    const std::shared_ptr<folly::SSLContext> sslContext = nullptr,
    std::optional<int> maybeIpTos = std::nullopt,
    const folly::SocketAddress& bindAddr = folly::AsyncSocket::anyAddress());

template <typename ClientType>
static thrift::KeyVals dumpAllWithThriftClientFromMultiple(
    const AreaId& area,
    const std::vector<std::unique_ptr<ClientType>>& clients,
    const std::string& prefix);

/*
 * Static method to retrieve loggable key-value information.
 *
 * @param logLevel - VLOG logging level
 * @param logStr - prefix to aid in logging
 * @param area - area with the key-val
 * @param key - key of key-val
 * @param val - thrift Value to log version, originator, ttl
 */
static void printKeyValInArea(
    int logLevel,
    const std::string& logStr,
    const std::string& areaTag,
    const std::string& key,
    const thrift::Value& val);

/*
 * This is the util method to merge the key-values publication to the existing
 * `kvStore` map, and return a publication made out of the updated values.
 *
 * High level speaking, we will perform:
 *  - 1st tie-breaker : version - prefer higher;
 *  - 2nd tie-breaker: originatorId - prefer higher;
 *  - 3rd tie-breaker: value(if exists) - prefer higher;
 *
 * @param kvStore - key-value map with current key-values in KVStore
 * @param keyVals - key-value map with key-values to merge in
 * @param filters - optional filters, matching keys in keyVals will be
 *                  merged in
 *
 * @return: a tuple of
 *  - key-value map obtained by merging data; publication made out of
 *    the updated values.
 *  - the statistics about reasons keys are NOT merged.
 */
thrift::KvStoreMergeResult mergeKeyValues(
    thrift::KeyVals& kvStore,
    const thrift::KeyVals& keyVals,
    std::optional<KvStoreFilters> const& filters = std::nullopt,
    std::optional<std::string> const& senderName = std::nullopt);

/*
 * Compare two thrift::Values to figure out which value is better to
 * use, it will compare following attributes in order
 * <version>, <orginatorId>, <value>, <ttl-version>
 *
 * @param v1 - first thrift::Value to compare
 * @param v2 - second thrift::Value to compare
 *
 * @return
 *  - ComparisonResult that represents which value is better
 *     FIRST  if v1 is better
 *     SECOND  if v2 is better
 *     TIED  if tied
 *     UNKNOWN  if unknown (can happen if value is missing -- only hash is
 *             provided)
 */
ComparisonResult compareValues(
    const thrift::Value& v1, const thrift::Value& v2);

// Dump the keys on which hashes differ from given keyVals
thrift::Publication dumpDifference(
    const std::string& area,
    const thrift::KeyVals& myKeyVal,
    const thrift::KeyVals& reqKeyVal);

// Dump the entries of my KV store whose keys match the filter
thrift::Publication dumpAllWithFilters(
    const std::string& area,
    const thrift::KeyVals& kvStore,
    const KvStoreFilters& kvFilters,
    bool doNotPublishValue = false);

// Dump the hashes of my KV store whose keys match the given prefix
// If prefix is the empty sting, the full hash store is dumped
thrift::Publication dumpHashWithFilters(
    const std::string& area,
    const thrift::KeyVals& kvStore,
    const KvStoreFilters& kvFilters);

// Update Time to expire filed in Publication
// If timeleft is below Constants::kTtlThreshold and removeAboutToExpire is
// true, erase keyVals
void updatePublicationTtl(
    const TtlCountdownQueue& ttlCountdownQueue,
    const std::chrono::milliseconds ttlDecr,
    thrift::Publication& thriftPub,
    const bool removeAboutToExpire = true);

/*
 * Check if TTL is valid.
 * Criteria: It must be infinite or positive number
 *
 * @param val - ttl value to check
 *
 * @return: true if satisfies the criteria, else false
 */
bool isValidTtl(int64_t val);

/*
 * Check if incoming value has valid version
 * Criteria: version must be equal or greater than existing version
 *
 * @param existingVersion - existing thrift::Value's version
 * @param incomingVal - incoming thrift::Value
 *
 * @return: true if satisfies the criteria, else false
 */
bool isValidVersion(
    const int64_t existingVersion, const thrift::Value& incomingVal);

/*
 * Check if what kind of merge type are we going to perform
 * check `MergeType` to see what types there are
 *
 * @param key - key to be merged
 * @param value - incoming thrift::Value
 * @param kvStore - existing key-value map
 * @param sender - the sender who sends the value
 * @param KvStoreMergeResult - record all the result and stats of this operation
 *
 * @return: MergeType - enum of the merging type
 */
MergeType getMergeType(
    const std::string& key,
    const thrift::Value& value,
    const thrift::KeyVals& kvStore,
    std::optional<std::string> const& sender,
    thrift::KvStoreMergeResult& result);

std::string getAreaTypeByAreaName(const std::string& area);
} // namespace openr

#include <openr/kvstore/KvStoreUtil-inl.h>
