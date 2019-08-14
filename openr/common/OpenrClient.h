/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>

namespace openr {

namespace detail {

static void
setCompressionTransform(apache::thrift::HeaderClientChannel* channel) {
  CHECK(channel);
  channel->setTransform(apache::thrift::transport::THeader::ZSTD_TRANSFORM);
}

static void
setCompressionTransform(apache::thrift::RocketClientChannel* /*channel*/) {
  CHECK(false) << "Transform is not supported on rocket client channel";
}
} // namespace detail

/**
 * Create client for OpenrCtrlCpp service over plain-text communication channel.
 *
 * Underneath client support multiple channel. Here we recommend to use two
 * channels based on your need.
 *
 * apache::thrift::HeaderClientChannel => This is default and widely used
 * channel. It doesn't support streaming APIs. However it support transparent
 * compression for data exchanges. This can be efficient for retrieving large
 * amount, of data like routes, topology, KvStore key/vals etc.
 *
 * apache::thrift::RocketClientChannel => This is new channel. It supports
 * streaming APIs. Use this if you need stream APIs.
 *
 */
template <typename ClientChannel = apache::thrift::HeaderClientChannel>
static std::unique_ptr<thrift::OpenrCtrlCppAsyncClient>
getOpenrCtrlPlainTextClient(
    folly::EventBase& evb,
    const folly::IPAddress& addr,
    int32_t port = Constants::kOpenrCtrlPort,
    std::chrono::milliseconds connectTimeout = Constants::kPlatformConnTimeout,
    std::chrono::milliseconds processingTimeout =
        std::chrono::milliseconds(10000)) {
  std::unique_ptr<thrift::OpenrCtrlCppAsyncClient> client;
  evb.runImmediatelyOrRunInEventBaseThreadAndWait([&]() mutable {
    // Create Socket
    // NOTE: It is possible to have caching for socket. We're not doing it as
    // we expect clients to be persistent/sticky.
    auto transport = apache::thrift::async::TAsyncSocket::UniquePtr(
        new apache::thrift::async::TAsyncSocket(
            &evb, folly::SocketAddress(addr, port), connectTimeout.count()),
        folly::DelayedDestruction::Destructor());

    // Create channel and set timeout
    auto channel = ClientChannel::newChannel(std::move(transport));
    channel->setTimeout(processingTimeout.count());

    // Enable compression for efficient transport when available. This will
    // incur CPU cost but it is insignificant for usual queries.
    detail::setCompressionTransform(channel.get());

    // Create client
    client =
        std::make_unique<thrift::OpenrCtrlCppAsyncClient>(std::move(channel));
  });

  return client;
}

// TODO: Add support for creating TLSSocket
//
// std::unique_ptr<thrift::OpenrCtrlCppAsyncClient>
// getOpenrCtrlSecureClient(...) {
//   ...
// }

} // namespace openr
