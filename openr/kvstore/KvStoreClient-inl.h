/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/zmq/Common.h>
#include <mutex>

#include <openr/kvstore/KvStore.h>

namespace openr {

// static
template <typename ThriftType>
ThriftType
KvStoreClient::parseThriftValue(thrift::Value const& value) {
  apache::thrift::CompactSerializer serializer;

  DCHECK(value.value.hasValue());

  auto buf =
    folly::IOBuf::wrapBufferAsValue(value.value->data(), value.value->size());
  return fbzmq::util::readThriftObj<ThriftType>(buf, serializer);
}

// static
template <typename ThriftType>
std::unordered_map<std::string, ThriftType>
KvStoreClient::parseThriftValues(
    std::unordered_map<std::string, thrift::Value> const& keyVals) {
  std::unordered_map<std::string, ThriftType> result;

  for (auto const& kv : keyVals) {
    // Here: kv.first is the key string, kv.second is thrift::Value
    result.emplace(
          kv.first, KvStoreClient::parseThriftValue<ThriftType>(kv.second));
  } // for

  return result;
}

// static
template <typename ThriftType>
std::pair<
    folly::Optional<std::unordered_map<std::string /* key */, ThriftType>>,
    std::vector<fbzmq::SocketUrl> /* unreached url */>
KvStoreClient::dumpAllWithPrefixMultipleAndParse(
    fbzmq::Context& context,
    const std::vector<fbzmq::SocketUrl>& kvStoreCmdUrls,
    const std::string& prefix,
    folly::Optional<std::chrono::milliseconds> recvTimeout,
    folly::Optional<int> maybeIpTos) {
  auto val = dumpAllWithPrefixMultiple(
      context, kvStoreCmdUrls, prefix, recvTimeout, maybeIpTos);
  if (not val.first) {
    return std::make_pair(folly::none, val.second);
  }
  return std::make_pair(parseThriftValues<ThriftType>(*val.first), val.second);
}

} // namespace openr
