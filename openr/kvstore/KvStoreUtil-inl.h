/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/gen/Base.h>
#include <openr/common/OpenrClient.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {

// static
template <typename ThriftType>
ThriftType
parseThriftValue(thrift::Value const& value) {
  apache::thrift::CompactSerializer serializer;

  DCHECK(value.value().has_value());

  auto buf = folly::IOBuf::wrapBufferAsValue(
      value.value()->data(), value.value()->size());
  return readThriftObj<ThriftType>(buf, serializer);
}

// static
template <typename ThriftType>
folly::F14FastMap<std::string, ThriftType>
parseThriftValues(thrift::KeyVals const& keyVals) {
  folly::F14FastMap<std::string, ThriftType> result;
  for (auto const& [key, val] : keyVals) {
    result.emplace(key, parseThriftValue<ThriftType>(val));
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
  const auto [res, unreachableAddrs] =
      dumpAllWithThriftClientFromMultiple<ClientType>(
          area,
          sockAddrs,
          keyPrefix,
          connectTimeout,
          processTimeout,
          sslContext,
          maybeIpTos,
          bindAddr);
  if (not res) {
    return std::make_pair(std::nullopt, unreachableAddrs);
  }
  return std::make_pair(parseThriftValues<ThriftType>(*res), unreachableAddrs);
}

// static
template <typename ThriftType, typename ClientType>
folly::F14FastMap<std::string /* key */, ThriftType>
dumpAllWithPrefixMultipleAndParse(
    folly::EventBase& evb,
    const AreaId& area,
    const std::vector<std::unique_ptr<ClientType>>& clients,
    const std::string& keyPrefix) {
  return parseThriftValues<ThriftType>(
      dumpAllWithThriftClientFromMultiple<ClientType>(
          evb, area, clients, keyPrefix));
}

void
printKeyValInArea(
    int logLevel,
    const std::string& logStr,
    const std::string& areaTag,
    const std::string& key,
    const thrift::Value& val) {
  VLOG(logLevel) << fmt::format(
      "{}{} [key: {}, v: {}, originatorId: {}, ttlVersion: {}, ttl: {}]",
      areaTag,
      logStr,
      key,
      *val.version(),
      *val.originatorId(),
      *val.ttlVersion(),
      *val.ttl());
}

// static method to dump KvStore key-val over multiple instances
template <typename ClientType>
std::pair<
    std::optional<thrift::KeyVals>,
    std::vector<folly::SocketAddress> /* unreachable addresses */>
dumpAllWithThriftClientFromMultiple(
    std::optional<AreaId> area,
    const std::vector<folly::SocketAddress>& sockAddrs,
    const std::string& keyPrefix,
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
  if (not keyPrefix.empty()) {
    params.keys() = {keyPrefix};
  }

  auto addrStrs =
      folly::gen::from(sockAddrs) |
      folly::gen::mapped([](const folly::SocketAddress& sockAddr) {
        return fmt::format(
            "[{}, {}]", sockAddr.getAddressStr(), sockAddr.getPort());
      }) |
      folly::gen::as<std::vector<std::string>>();

  VLOG(1) << "Dump kvStore key-vals from: " << folly::join(",", addrStrs)
          << ". Required SSL secure connection: " << std::boolalpha
          << (sslContext != nullptr);

  auto startTime = std::chrono::steady_clock::now();
  for (auto const& sockAddr : sockAddrs) {
    std::unique_ptr<ClientType> client{nullptr};
    if (sslContext) {
      VLOG(3) << "Try to connect Open/R SSL secure client.";
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
      VLOG(3) << "Try to connect Open/R plain-text client.";
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

    VLOG(3) << "Successfully connected to Open/R with addr: "
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
        VLOG(1) << "Merge key-vals from " << results.size()
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

            VLOG(3) << "Received kvstore publication with: " << keyVals.size()
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

  VLOG(1) << "Took: " << elapsedTime << "ms to retrieve KvStore snapshot";

  return std::make_pair(merged, unreachableAddrs);
}

// static method to dump KvStore key-val over multiple instances
template <typename ClientType>
thrift::KeyVals
dumpAllWithThriftClientFromMultiple(
    folly::EventBase& evb,
    const AreaId& area,
    const std::vector<std::unique_ptr<ClientType>>& clients,
    const std::string& keyPrefix) {
  std::vector<folly::SemiFuture<thrift::Publication>> calls;
  thrift::KeyVals merged;

  thrift::KeyDumpParams params;
  if (not keyPrefix.empty()) {
    params.keys() = {keyPrefix};
  }

  auto startTime = std::chrono::steady_clock::now();
  for (const auto& client : clients) {
    calls.emplace_back(
        client->semifuture_getKvStoreKeyValsFilteredArea(params, area));
  }

  // loop semifuture collection to merge all values
  for (const auto& result : folly::collectAll(calls).via(&evb).getVia(&evb)) {
    // folly::Try will contain either value or exception
    if (result.hasException()) {
      LOG(ERROR) << "Exception: " << folly::exceptionStr(result.exception());
    } else if (result.hasValue()) {
      auto keyVals = *result.value().keyVals();
      const auto deltaPub = *mergeKeyValues(merged, keyVals).keyVals();

      VLOG(3) << "Received kvstore publication with: " << keyVals.size()
              << " key-vals. Incurred " << deltaPub.size()
              << " key-val updates.";
    }
  }

  // record time used to fetch from all Open/R instances
  const auto elapsedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime)
          .count();

  VLOG(1) << "Took: " << elapsedTime << "ms to retrieve KvStore snapshot";

  return merged;
}

} // namespace openr
