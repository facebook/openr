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
#include <thrift/lib/cpp2/async/PooledRequestChannel.h>
#include <thrift/lib/cpp2/async/ReconnectingRequestChannel.h>
#include <thrift/lib/cpp2/async/RetryingRequestChannel.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

namespace openr {

struct OpenrClientOpts {
  /* Populate to use TLS. */
  std::shared_ptr<folly::SSLContext> sslContext = nullptr;
  /* Default connect timeout assumes TLS and is more conservative; for plaintext
   * consider the shorter timeout available in Constants. */
  std::chrono::milliseconds connectTimeout = Constants::kServiceConnSSLTimeout;
  std::chrono::milliseconds processingTimeout = Constants::kServiceProcTimeout;
  folly::SocketAddress bindAddr = folly::AsyncSocket::anyAddress();
  /* How many times we retry non-application failures before returning an error.
   * This does not guarantee that the backend did not receive your request so
   * should never be set for non-idempotent operations */
  int numTransportFailureRetries = 0;
  /* This does not alter the underlying threadpool (which is sized automatically
   * based on core count) but how many of those threads we multiplex onto.
   * Pick 0 to use the entire executor threadpool. */
  size_t numIOThreads = 1;
  std::optional<int> maybeIpTos = std::nullopt;
  /* Whether we try to enable TCP keepalive. Failures are unlikely but this
   * is technically best-effort and the client will proceed if it can't apply
   * the relevant configuration to the underlying socket. */
  bool enableKeepAlive = false;
};

namespace detail {
/* Sets compression on innerermost (probably Rocket) channel */
void setCompressionTransform(apache::thrift::ClientChannel* channel);

/* Produces a folly-compatible version of IP ToS. */
folly::SocketOptionMap getSocketOptionMap(std::optional<int> maybeIpTos);

/* Tries to enable TCP keepalive. */
void tryEnableKeepAliveForSocket(folly::AsyncSocket* socket);

/* Returns the number of threads in the folly global I/O executor. */
size_t getFollyIOPoolConcurrency();

/* Returns a factory function called on each reconnection attempt to
 * produce a new Rocket channel using a new socket */
apache::thrift::ReconnectingRequestChannel::ImplCreatorWithCallback
getInnerSocketChannelFactory(
    folly::IPAddress addr, int32_t port, OpenrClientOpts opts);

} // namespace detail

/*
 * Templated method to create a client for thrift service over tls or
 * plain-text communication channel. Different clients for different services
 * can be used. The returned client will recover automatically from connection
 * errors, can optionally retry and can be used in arbitrary threads.
 *
 * For example,
 *  - thrift::OpenrCtrlCppAsyncClient -> OpenrCtrlCpp service
 *  - thrift::KvStoreServiceAsyncClient -> KvStoreService
 *
 * Pass a valid SSLContext in the options if you want TLS.
 */
template <typename ClientType>
std::shared_ptr<apache::thrift::Client<ClientType>>
getOpenrClient(
    const folly::IPAddress& addr,
    int32_t port = Constants::kOpenrCtrlPort,
    const OpenrClientOpts& opts = {}) {
  // 0 scales to entire executor
  const size_t threads = opts.numIOThreads == 0
      ? detail::getFollyIOPoolConcurrency()
      : opts.numIOThreads;
  /* Top-level channel gives us thread-safety + reuse of appropriate
   * folly threadpools. */
  auto channel = apache::thrift::PooledRequestChannel::newChannel(
      [addr, port, opts](folly::EventBase& evb) {
        // Retries transport errors
        return apache::thrift::RetryingRequestChannel::newChannel(
            evb,
            static_cast<int>(opts.numTransportFailureRetries),
            // Rebuilds internal Rocket channel if socket isn't usable
            apache::thrift::ReconnectingRequestChannel::newChannel(
                evb, detail::getInnerSocketChannelFactory(addr, port, opts)));
      },
      threads);
  return std::make_shared<apache::thrift::Client<ClientType>>(
      std::move(channel));
}

/*
 * This is templated method to create client for thrift service over plain-text
 * communication channel. Different clients for different services can be used.
 *
 * For example,
 *  - thrift::OpenrCtrlCppAsyncClient -> OpenrCtrlCpp service
 *  - thrift::KvStoreServiceAsyncClient -> KvStoreService
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
  /*
   * NOTE: It is possible to have caching for socket. We're not doing it as
   * we expect clients to be persistent/sticky.
   */
  std::unique_ptr<ClientType> client{nullptr};

  evb.runImmediatelyOrRunInEventBaseThreadAndWait([&]() mutable {
    /*
     * Create a new UNCONNECTED AsyncSocket
     * ATTN: don't change contructor flavor to connect automatically.
     */
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
      detail::tryEnableKeepAliveForSocket(transport.get());
    }

    // Create channel and set timeout
    auto channel = ClientChannel::newChannel(std::move(transport));
    channel->setTimeout(processingTimeout.count());

    /*
     * Enable compression for efficient transport when available. This will
     * incur CPU cost but it is insignificant for usual queries.
     */
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
  /*
   * NOTE: It is possible to have caching for socket. We're not doing it as
   * we expect clients to be persistent/sticky.
   */
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
      detail::tryEnableKeepAliveForSocket(transport.get());
    }

    // Create channel and set timeout
    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(transport));
    channel->setTimeout(processingTimeout.count());

    /*
     * Enable compression for efficient transport when available. This will
     * incur CPU cost but it is insignificant for usual queries.
     */
    detail::setCompressionTransform(channel.get());

    // Create client
    client = std::make_unique<ClientType>(std::move(channel));
  });

  return client;
}

} // namespace openr
