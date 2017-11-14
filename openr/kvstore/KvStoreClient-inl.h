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
std::unordered_map<std::string, ThriftType>
KvStoreClient::parseThriftValues(
    std::unordered_map<std::string, thrift::Value> const& keyVals) {
  apache::thrift::CompactSerializer serializer;
  std::unordered_map<std::string, ThriftType> result;

  for (auto const& kv : keyVals) {
    // Here: kv.first is the key string, kv.second is thrift::Value

    DCHECK(kv.second.value.hasValue());

    auto buf = folly::IOBuf::wrapBufferAsValue(
        kv.second.value->data(), kv.second.value->size());
    auto deser = fbzmq::util::readThriftObj<ThriftType>(buf, serializer);
    result.emplace(kv.first, std::move(deser));
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
    folly::Optional<std::chrono::milliseconds> recvTimeout) {
  auto val =
      dumpAllWithPrefixMultiple(context, kvStoreCmdUrls, prefix, recvTimeout);
  if (not val.first) {
    return std::make_pair(folly::none, val.second);
  }
  return std::make_pair(parseThriftValues<ThriftType>(*val.first), val.second);
}

} // namespace openr
