/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>

namespace openr {

std::unique_ptr<thrift::OpenrCtrlCppAsyncClient>
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
    // we expect clients to be persistent.
    auto transport = apache::thrift::async::TAsyncSocket::UniquePtr(
        new apache::thrift::async::TAsyncSocket(
            &evb, folly::SocketAddress(addr, port), connectTimeout.count()),
        folly::DelayedDestruction::Destructor());

    // Create channel and set timeout
    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(transport));
    channel->setTimeout(processingTimeout.count());

    // NOTE: Enable transform for compression when available with
    // RocketClientChannel
    // Enable compression for efficient transport. This will incur CPU cost but
    // it is negligible
    // channel->setTransform(apache::thrift::transport::THeader::ZSTD_TRANSFORM);

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
