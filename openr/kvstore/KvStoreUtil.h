/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncSocket.h>
#include <openr/common/Constants.h>
#include <openr/common/Types.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

namespace openr {

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
    std::unordered_map<std::string, thrift::Value> const& keyVals);

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
template <typename ThriftType>
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
static std::pair<
    std::optional<std::unordered_map<std::string, thrift::Value>>,
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

/*
 * Static method to retrieve loggable key-value information.
 *
 * @param logLevel - VLOG logging level
 * @param prefixStr - prefix to aid in logging
 * @param area - area with the key-val
 * @param key - key of key-val
 * @param val - thrift Value to log version, originator, ttl
 */
static void printKeyValInArea(
    int logLevel,
    const std::string& prefixStr,
    const std::string& area,
    const std::string& key,
    const thrift::Value& val);

/*
 * Static method to precess the key-values publication, attempt to merge it
 in
 * the existing map, and return a publication made out of the updated values.
 *
 * @param kvStore - key-value map with current key-values in KVStore
 * @param keyVals - key-value map with key-values to merge in
 * @param filters - optional filters, matching keys in keyVals will be
                    merged in
 *
 * @return
 *  - key-value map obtained by merging data; publication made out of
 *    the updated values
 */
std::unordered_map<std::string, thrift::Value> mergeKeyValues(
    std::unordered_map<std::string, thrift::Value>& kvStore,
    std::unordered_map<std::string, thrift::Value> const& keyVals,
    std::optional<KvStoreFilters> const& filters = std::nullopt);

std::optional<openr::KvStoreFilters> getKvStoreFilters(
    std::shared_ptr<const openr::Config> config);

/*
 * Compare two thrift::Values to figure out which value is better to
 * use, it will compare following attributes in order
 * <version>, <orginatorId>, <value>, <ttl-version>
 *
 * @param v1 - first thrift::Value to compare
 * @param v2 - second thrift::Value to compare
 *
 * @return
 *  - int that represents which value is better
 *      1  if v1 is better
 *     -1  if v2 is better
 *      0  if equal
 *     -2  if unknown (can happen if value is missing -- only hash is provided)
 */
int compareValues(const thrift::Value& v1, const thrift::Value& v2);

} // namespace openr

#include <openr/kvstore/KvStoreUtil-inl.h>
