/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ranges>
#include <span>

#include <folly/gen/Base.h>
#include <folly/logging/xlog.h>
#include <openr/common/OpenrClient.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {
namespace detail {

template <typename ThriftType>
ThriftType
parseThriftValue(thrift::Value const& value) {
  apache::thrift::CompactSerializer serializer;

  DCHECK(value.value().has_value());

  auto buf = folly::IOBuf::wrapBufferAsValue(
      value.value()->data(), value.value()->size());
  return readThriftObj<ThriftType>(buf, serializer);
}

} // namespace detail

template <
    typename ThriftType,
    SizedKeyValueRange<std::string, thrift::Value> PairRange>
folly::F14FastMap<std::string, ThriftType>
parseThriftValues(PairRange const& keyVals) {
  folly::F14FastMap<std::string, ThriftType> result;
  result.reserve(std::ranges::size(keyVals));
  for (auto const& [key, val] : keyVals) {
    result.emplace(key, detail::parseThriftValue<ThriftType>(val));
  }
  return result;
}

// static
template <typename ThriftType, typename ClientType>
std::pair<
    std::optional<folly::F14FastMap<std::string /* key */, ThriftType>>,
    std::vector<folly::SocketAddress> /* unreached url */>
dumpAllWithPrefixMultipleAndParse(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& keyPrefix,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds processTimeout,
    const std::shared_ptr<folly::SSLContext> sslContext,
    std::optional<int> maybeIpTos /* std::nullopt */,
    const folly::SocketAddress&
        bindAddr /* folly::AsyncSocket::anyAddress()*/) {
  auto prefixSpan = keyPrefix.empty()
      ? std::span<const std::string>{}
      : std::span<const std::string>{&keyPrefix, 1};
  const auto [res, unreachableAddrs] =
      dumpAllWithThriftClientFromMultiple<ClientType>(
          area,
          sockAddrs,
          prefixSpan,
          connectTimeout,
          processTimeout,
          sslContext,
          maybeIpTos,
          bindAddr);
  if (!res) {
    return std::make_pair(std::nullopt, unreachableAddrs);
  }
  return std::make_pair(parseThriftValues<ThriftType>(*res), unreachableAddrs);
}

template <typename ThriftType, typename ClientType>
folly::F14FastMap<std::string /* key */, ThriftType>
dumpAllWithPrefixMultipleAndParse(
    folly::EventBase& evb,
    const AreaId& area,
    const std::vector<std::unique_ptr<ClientType>>& clients,
    const std::string& keyPrefix) {
  auto prefixSpan = keyPrefix.empty()
      ? std::span<const std::string>{}
      : std::span<const std::string>{&keyPrefix, 1};
  return parseThriftValues<ThriftType>(
      dumpAllWithThriftClientFromMultiple<ClientType>(
          evb, area, clients, prefixSpan)
          .keyVals);
}

template <typename ClientType, StringRange KeyPrefixes>
std::pair<
    std::optional<thrift::KeyVals>,
    std::vector<folly::SocketAddress> /* unreachable addresses */>
dumpAllWithThriftClientFromMultiple(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const KeyPrefixes& keyPrefixes,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds processTimeout,
    const std::shared_ptr<folly::SSLContext> sslContext,
    std::optional<int> maybeIpTos /* std::nullopt */,
    const folly::SocketAddress&
        bindAddr /* folly::AsyncSocket::anyAddress()*/) {
  folly::EventBase evb;
  std::vector<folly::SemiFuture<thrift::Publication>> calls;
  thrift::KeyVals merged;
  std::vector<folly::SocketAddress> unreachableAddrs;

  thrift::KeyDumpParams params;
  if (!keyPrefixes.empty()) {
    params.keys() = {keyPrefixes.begin(), keyPrefixes.end()};
  }

  if (XLOG_IS_ON(DBG2)) {
    auto addrStrs =
        folly::gen::from(sockAddrs) |
        folly::gen::mapped([](const folly::SocketAddress& sockAddr) {
          return fmt::format(
              "[{}, {}]", sockAddr.getAddressStr(), sockAddr.getPort());
        }) |
        folly::gen::as<std::vector<std::string>>();

    XLOG(DBG2) << "Dump kvStore key-vals from: " << folly::join(",", addrStrs)
               << ". Required SSL secure connection: " << std::boolalpha
               << (sslContext != nullptr);
  }

  auto startTime = std::chrono::steady_clock::now();
  for (auto const& sockAddr : sockAddrs) {
    std::unique_ptr<ClientType> client{nullptr};
    if (sslContext) {
      XLOG(DBG3) << "Try to connect Open/R SSL secure client.";
      try {
        client = getOpenrCtrlSecureClient<ClientType>(
            evb,
            sslContext,
            folly::IPAddress(sockAddr.getAddressStr()),
            sockAddr.getPort(),
            connectTimeout,
            processTimeout,
            bindAddr,
            maybeIpTos);
      } catch (const std::exception& ex) {
        LOG(ERROR)
            << "Failed to connect to Open/R instance at: "
            << sockAddr.getAddressStr()
            << " via secure client. Exception: " << folly::exceptionStr(ex);
      }
    }

    // Cannot connect to Open/R via secure client. Try plain-text client
    if (!client) {
      XLOG(DBG3) << "Try to connect Open/R plain-text client.";
      try {
        client = getOpenrCtrlPlainTextClient<ClientType>(
            evb,
            folly::IPAddress(sockAddr.getAddressStr()),
            sockAddr.getPort(),
            connectTimeout,
            processTimeout,
            bindAddr,
            maybeIpTos);
      } catch (const std::exception& ex) {
        LOG(ERROR)
            << "Failed to connect to Open/R instance at: "
            << sockAddr.getAddressStr()
            << "via plain-text client. Exception: " << folly::exceptionStr(ex);
      }
    }

    // Cannot connect to Open/R via either plain-text client or secured client
    if (!client) {
      unreachableAddrs.emplace_back(sockAddr);
      continue;
    }

    XLOG(DBG3) << "Successfully connected to Open/R with addr: "
               << sockAddr.getAddressStr();

    calls.emplace_back(
        area ? client->semifuture_getKvStoreKeyValsFilteredArea(params, *area)
             : client->semifuture_getKvStoreKeyValsFiltered(params));
  }

  // can't connect to ANY single Open/R instance
  if (calls.empty()) {
    return std::make_pair(std::nullopt, unreachableAddrs);
  }

  folly::collectAll(calls).via(&evb).thenValue(
      [&](std::vector<folly::Try<openr::thrift::Publication>>&& results) {
        XLOG(DBG1) << "Merge key-vals from " << results.size()
                   << " different Open/R instances.";

        // loop semifuture collection to merge all values
        for (auto& result : results) {
          // folly::Try will contain either value or exception
          if (result.hasException()) {
            LOG(ERROR) << "Exception: "
                       << folly::exceptionStr(result.exception());
          } else if (result.hasValue()) {
            auto keyVals = *result.value().keyVals();
            const auto deltaPub = *mergeKeyValues(merged, keyVals).keyVals();

            XLOG(DBG3) << "Received kvstore publication with: "
                       << keyVals.size() << " key-vals. Incurred "
                       << deltaPub.size() << " key-val updates.";
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

  XLOG(DBG1) << "Took: " << elapsedTime << "ms to retrieve KvStore snapshot";

  return std::make_pair(merged, unreachableAddrs);
}

// dump KvStore key-val over multiple instances
template <typename ClientType, StringRange KeyPrefixes>
KvStoreDumpWithConnectionMeta
dumpAllWithThriftClientFromMultiple(
    folly::EventBase& evb,
    const AreaId& area,
    const std::vector<std::unique_ptr<ClientType>>& clients,
    const KeyPrefixes& keyPrefixes) {
  std::vector<folly::SemiFuture<thrift::Publication>> calls;
  thrift::KeyVals merged;

  thrift::KeyDumpParams params;
  if (!keyPrefixes.empty()) {
    params.keys() = {keyPrefixes.begin(), keyPrefixes.end()};
  }

  auto startTime = std::chrono::steady_clock::now();
  for (const auto& client : clients) {
    calls.emplace_back(
        client->semifuture_getKvStoreKeyValsFilteredArea(params, area));
  }

  uint32_t failures = 0;
  // loop semifuture collection to merge all values
  for (const auto& result : folly::collectAll(calls).via(&evb).getVia(&evb)) {
    // folly::Try will contain either value or exception
    if (result.hasException()) {
      LOG(ERROR) << "Exception: " << folly::exceptionStr(result.exception());
      failures++;
    } else if (result.hasValue()) {
      auto keyVals = *result.value().keyVals();
      const auto deltaPub = *mergeKeyValues(merged, keyVals).keyVals();

      XLOG(DBG3) << "Received kvstore publication with: " << keyVals.size()
                 << " key-vals. Incurred " << deltaPub.size()
                 << " key-val updates.";
    }
  }

  // record time used to fetch from all Open/R instances
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime)
          .count();

  XLOG(DBG1) << "Took: " << elapsedTime << "ms to retrieve KvStore snapshot";

  return {merged, failures};
}

} // namespace openr
