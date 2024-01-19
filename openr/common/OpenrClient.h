/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/logging/xlog.h>

#include <openr/common/Constants.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

namespace openr {

namespace detail {

static void
setCompressionTransform(apache::thrift::ClientChannel* channel) {
  CHECK(channel);
  apache::thrift::CompressionConfig compressionConfig;
  compressionConfig.codecConfig().ensure().set_zstdConfig();
  channel->setDesiredCompressionConfig(compressionConfig);
}

/*
 * Build OptionMap for client socket connection
 */
static folly::SocketOptionMap
getSocketOptionMap(std::optional<int> maybeIpTos) {
  folly::SocketOptionMap optionMap = folly::emptySocketOptionMap;
  if (maybeIpTos.has_value()) {
    folly::SocketOptionKey v6Opts = {IPPROTO_IPV6, IPV6_TCLASS};
    optionMap.emplace(v6Opts, maybeIpTos.value());
  }
  return optionMap;
}

} // namespace detail

/*
 * This is templated method to create client for thrift service over plain-text
 * communication channel. Different clients for different services can be used.
 *
 * For example,
 *  - thrift::OpenrCtrlCppAsyncClient -> OpenrCtrlCpp service
 *  - thrift::KvStoreServicAsyncClient -> KvStoreService
 *
 * Underneath client support multiple channel. Here we recommend to use
 * apache::thrift::RocketClientChannel, which supports streaming APIs.
 */
template <
    typename ClientType,
    typename ClientChannel = apache::thrift::RocketClientChannel>
static std::unique_ptr<ClientType>
getOpenrCtrlPlainTextClient(
    folly::EventBase& evb,
    const folly::IPAddress& addr,
    int32_t port = Constants::kOpenrCtrlPort,
    std::chrono::milliseconds connectTimeout = Constants::kServiceConnTimeout,
    std::chrono::milliseconds processingTimeout =
        Constants::kServiceProcTimeout,
    const folly::SocketAddress& bindAddr = folly::AsyncSocket::anyAddress(),
    std::optional<int> maybeIpTos = std::nullopt,
    bool enableKeepAlive = false) {
  // NOTE: It is possible to have caching for socket. We're not doing it as
  // we expect clients to be persistent/sticky.
  std::unique_ptr<ClientType> client{nullptr};

  evb.runImmediatelyOrRunInEventBaseThreadAndWait([&]() mutable {
    // Create a new UNCONNECTED AsyncSocket
    // ATTN: don't change contructor flavor to connect automatically.
    const folly::SocketAddress sa(addr, port);
    auto transport = folly::AsyncSocket::newSocket(&evb);

    // Establish connection
    transport->connect(
        nullptr,
        sa,
        connectTimeout.count(),
        detail::getSocketOptionMap(maybeIpTos),
        bindAddr);

    if (enableKeepAlive) {
      /*
       * Set up socket keepalive options so that we break the connection in a
       * timely manner in the case of ungraceful disconnect when FIN/RST is not
       * received from the remote end.
       */
      int optval = 1;
      if (transport->setSockOpt(SOL_SOCKET, SO_KEEPALIVE, &optval) != 0) {
        XLOG(WARNING) << fmt::format(
            "Could not set SO_KEEPALIVE flag on socket. Error: {}", errno);
      }

      // The time (in seconds) between individual keepalive probes
      int interval = Constants::kThriftClientKeepAliveInterval.count();
      if (transport->setSockOpt(IPPROTO_TCP, TCP_KEEPINTVL, &interval) != 0) {
        XLOG(WARNING) << fmt::format(
            "Could not set TCP_KEEPINTVL value on socket. Error: {}", errno);
      }
      XLOG(INFO) << fmt::format(
          "Successfully set TCP socket keepalive with interval: {}", interval);
    }

    // Create channel and set timeout
    auto channel = ClientChannel::newChannel(std::move(transport));
    channel->setTimeout(processingTimeout.count());

    // Enable compression for efficient transport when available. This will
    // incur CPU cost but it is insignificant for usual queries.
    detail::setCompressionTransform(channel.get());

    // Create client
    client = std::make_unique<ClientType>(std::move(channel));
  });

  return client;
}

/*
 * Create secured client for OpenrCtrlCpp service over AsyncSSLSocket.
 */
template <typename ClientType>
static std::unique_ptr<ClientType>
getOpenrCtrlSecureClient(
    folly::EventBase& evb,
    const std::shared_ptr<folly::SSLContext> sslContext,
    const folly::IPAddress& addr,
    int32_t port = Constants::kOpenrCtrlPort,
    std::chrono::milliseconds connectTimeout =
        Constants::kServiceConnSSLTimeout,
    std::chrono::milliseconds processingTimeout =
        Constants::kServiceProcTimeout,
    const folly::SocketAddress& bindAddr = folly::AsyncSocket::anyAddress(),
    std::optional<int> maybeIpTos = std::nullopt,
    bool enableKeepAlive = false) {
  // NOTE: It is possible to have caching for socket. We're not doing it as
  // we expect clients to be persistent/sticky.
  std::unique_ptr<ClientType> client{nullptr};

  evb.runImmediatelyOrRunInEventBaseThreadAndWait([&]() mutable {
    // Create a new UNCONNECTED AsyncSocket
    const folly::SocketAddress sa(addr, port);

    auto transport = folly::AsyncSocket::UniquePtr(
        new folly::AsyncSSLSocket(std::move(sslContext), &evb));

    // Establish connection
    transport->connect(
        nullptr,
        sa,
        connectTimeout.count(),
        detail::getSocketOptionMap(maybeIpTos),
        bindAddr);

    if (enableKeepAlive) {
      /*
       * Set up socket keepalive options so that we break the connection in a
       * timely manner in the case of ungraceful disconnect when FIN/RST is not
       * received from the remote end.
       */
      int optval = 1;
      if (transport->setSockOpt(SOL_SOCKET, SO_KEEPALIVE, &optval) != 0) {
        XLOG(WARNING) << fmt::format(
            "Could not set SO_KEEPALIVE flag on socket. Error: {}", errno);
      }

      // The time (in seconds) between individual keepalive probes
      int interval = Constants::kThriftClientKeepAliveInterval.count();
      if (transport->setSockOpt(IPPROTO_TCP, TCP_KEEPINTVL, &interval) != 0) {
        XLOG(WARNING) << fmt::format(
            "Could not set TCP_KEEPINTVL value on socket. Error: {}", errno);
      }
      XLOG(INFO) << fmt::format(
          "Successfully set TCP socket keepalive with interval: {}", interval);
    }

    // Create channel and set timeout
    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(transport));
    channel->setTimeout(processingTimeout.count());

    // Enable compression for efficient transport when available. This will
    // incur CPU cost but it is insignificant for usual queries.
    detail::setCompressionTransform(channel.get());

    // Create client
    client = std::make_unique<ClientType>(std::move(channel));
  });

  return client;
}

} // namespace openr
