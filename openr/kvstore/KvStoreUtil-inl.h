/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/OpenrClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

// static
template <typename ThriftType>
ThriftType
parseThriftValue(thrift::Value const& value) {
  apache::thrift::CompactSerializer serializer;

  DCHECK(value.value_ref().has_value());

  auto buf = folly::IOBuf::wrapBufferAsValue(
      value.value_ref()->data(), value.value_ref()->size());
  return fbzmq::util::readThriftObj<ThriftType>(buf, serializer);
}

// static
template <typename ThriftType>
std::unordered_map<std::string, ThriftType>
parseThriftValues(
    std::unordered_map<std::string, thrift::Value> const& keyVals) {
  std::unordered_map<std::string, ThriftType> result;

  for (auto const& kv : keyVals) {
    // Here: kv.first is the key string, kv.second is thrift::Value
    result.emplace(kv.first, parseThriftValue<ThriftType>(kv.second));
  } // for

  return result;
}

// static
template <typename ThriftType>
std::pair<
    std::optional<std::unordered_map<std::string /* key */, ThriftType>>,
    std::vector<fbzmq::SocketUrl> /* unreached url */>
dumpAllWithPrefixMultipleAndParse(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& keyPrefix,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds processTimeout,
    std::optional<int> maybeIpTos /* std::nullopt */,
    const folly::SocketAddress&
        bindAddr /* folly::AsyncSocket::anyAddress()*/) {
  auto val = dumpAllWithThriftClientFromMultiple(
      area,
      sockAddrs,
      keyPrefix,
      connectTimeout,
      processTimeout,
      maybeIpTos,
      bindAddr);
  if (not val.first) {
    return std::make_pair(std::nullopt, val.second);
  }
  return std::make_pair(parseThriftValues<ThriftType>(*val.first), val.second);
}

// static method to dump KvStore key-val over multiple instances
std::pair<
    std::optional<std::unordered_map<std::string /* key */, thrift::Value>>,
    std::vector<fbzmq::SocketUrl> /* unreached url */>
dumpAllWithThriftClientFromMultiple(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& keyPrefix,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds processTimeout,
    std::optional<int> maybeIpTos /* std::nullopt */,
    const folly::SocketAddress&
        bindAddr /* folly::AsyncSocket::anyAddress()*/) {
  folly::EventBase evb;
  std::vector<folly::SemiFuture<thrift::Publication>> calls;
  std::unordered_map<std::string, thrift::Value> merged;
  std::vector<fbzmq::SocketUrl> unreachedUrls;

  thrift::KeyDumpParams params;
  *params.prefix_ref() = keyPrefix;
  if (not keyPrefix.empty()) {
    params.keys_ref() = {keyPrefix};
  }

  auto addrStrs =
      folly::gen::from(sockAddrs) |
      folly::gen::mapped([](const folly::SocketAddress& sockAddr) {
        return folly::sformat(
            "[{}, {}]", sockAddr.getAddressStr(), sockAddr.getPort());
      }) |
      folly::gen::as<std::vector<std::string>>();

  LOG(INFO) << "Dump kvStore key-vals from: " << folly::join(",", addrStrs);

  auto startTime = std::chrono::steady_clock::now();
  for (auto const& sockAddr : sockAddrs) {
    std::unique_ptr<thrift::OpenrCtrlCppAsyncClient> client{nullptr};
    try {
      client = getOpenrCtrlPlainTextClient(
          evb,
          folly::IPAddress(sockAddr.getAddressStr()),
          sockAddr.getPort(),
          connectTimeout,
          processTimeout,
          bindAddr,
          maybeIpTos);
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Failed to connect to Open/R instance at address of: "
                 << sockAddr.getAddressStr()
                 << ". Exception: " << folly::exceptionStr(ex);
    }
    if (!client) {
      unreachedUrls.push_back(fbzmq::SocketUrl{sockAddr.getAddressStr()});
      continue;
    }

    VLOG(3) << "Successfully connected to Open/R with addr: "
            << sockAddr.getAddressStr();

    calls.emplace_back(
        area ? client->semifuture_getKvStoreKeyValsFilteredArea(params, *area)
             : client->semifuture_getKvStoreKeyValsFiltered(params));
  }

  // can't connect to ANY single Open/R instance
  if (calls.empty()) {
    return std::make_pair(std::nullopt, unreachedUrls);
  }

  folly::collectAll(calls).via(&evb).thenValue(
      [&](std::vector<folly::Try<openr::thrift::Publication>>&& results) {
        LOG(INFO) << "Merge key-vals from " << results.size()
                  << " different Open/R instances.";

        // loop semifuture collection to merge all values
        for (auto& result : results) {
          // folly::Try will contain either value or exception
          // Do NOT CHECK(result.hasValue()) since exception can happen.
          if (result.hasException()) {
            LOG(ERROR) << "Exception: "
                       << folly::exceptionStr(result.exception());
          } else if (result.hasValue()) {
            auto keyVals = *result.value().keyVals_ref();
            const auto deltaPub = KvStore::mergeKeyValues(merged, keyVals);

            LOG(INFO) << "Received kvstore publication with: " << keyVals.size()
                      << " key-vals. Incurred " << deltaPub.size()
                      << " key-val updates.";
          }
        }
        evb.terminateLoopSoon();
      });

  // magic happens here
  evb.loopForever();

  // record time used to fetch from all Open/R instances
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime)
          .count();

  LOG(INFO) << "Took: " << elapsedTime << "ms to retrieve KvStore snapshot";

  return std::make_pair(merged, unreachedUrls);
}

} // namespace openr
