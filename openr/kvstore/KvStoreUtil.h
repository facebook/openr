/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncSocket.h>
#include <openr/common/Constants.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>

namespace openr {

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
    std::optional<std::unordered_map<std::string /* key */, thrift::Value>>,
    std::vector<folly::SocketAddress> /* unreachable url */>
dumpAllWithThriftClientFromMultiple(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& prefix,
    std::chrono::milliseconds connectTimeout = Constants::kServiceConnTimeout,
    std::chrono::milliseconds processTimeout = Constants::kServiceProcTimeout,
    const std::shared_ptr<folly::SSLContext> sslContext = nullptr,
    std::optional<int> maybeIpTos = std::nullopt,
    const folly::SocketAddress& bindAddr = folly::AsyncSocket::anyAddress());

} // namespace openr

#include <openr/kvstore/KvStoreUtil-inl.h>
