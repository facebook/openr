/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/OpenrClient.h"

#include "folly/executors/GlobalExecutor.h"

namespace openr::detail {

void
setCompressionTransform(apache::thrift::ClientChannel* channel) {
  CHECK(channel);
  apache::thrift::CompressionConfig compressionConfig;
  compressionConfig.codecConfig().ensure().set_zstdConfig();
  channel->setDesiredCompressionConfig(compressionConfig);
}

folly::SocketOptionMap
getSocketOptionMap(std::optional<int> maybeIpTos) {
  folly::SocketOptionMap optionMap = folly::emptySocketOptionMap;
  if (maybeIpTos.has_value()) {
    folly::SocketOptionKey v6Opts = {IPPROTO_IPV6, IPV6_TCLASS};
    optionMap.emplace(v6Opts, maybeIpTos.value());
  }
  return optionMap;
}

void
tryEnableKeepAliveForSocket(folly::AsyncSocket* socket) {
  /*
   * Set up socket keepalive options so that we break the connection in a
   * timely manner in the case of ungraceful disconnect when FIN/RST is not
   * received from the remote end.
   */
  int optval = 1;
  if (socket->setSockOpt(SOL_SOCKET, SO_KEEPALIVE, &optval) != 0) {
    // Pointless to try to set second param if first has failed
    XLOGF(
        WARNING, "Could not set SO_KEEPALIVE flag on socket. Error: {}", errno);
    return;
  }

  /* The time (in seconds) between individual keepalive probes */
  int interval = Constants::kThriftClientKeepAliveInterval.count();
  if (socket->setSockOpt(IPPROTO_TCP, TCP_KEEPINTVL, &interval) != 0) {
    XLOGF(
        WARNING,
        "Could not set TCP_KEEPINTVL value on socket. Error: {}",
        errno);
    return;
  }
  XLOGF(
      INFO,
      "Successfully set TCP socket keepalive with interval: {}",
      interval);
}

size_t
getFollyIOPoolConcurrency() {
  /* We can't get the threadcount from the executor without a fragile
   * downcast that could break at any time. This flag by contrast is
   * part of the public interface of every FB binary. */
  const size_t nthreads = FLAGS_folly_global_io_executor_threads;
  return nthreads ? nthreads : folly::available_concurrency();
}

apache::thrift::ReconnectingRequestChannel::ImplCreatorWithCallback
getInnerSocketChannelFactory(
    folly::IPAddress addr, int32_t port, OpenrClientOpts opts) {
  return
      [addr = std::move(addr), port, opts = std::move(opts)](
          folly::EventBase& innerEvb, folly::AsyncSocket::ConnectCallback& cb) {
        folly::AsyncSocket::UniquePtr socket = opts.sslContext == nullptr
            ? folly::AsyncSocket::newSocket(&innerEvb)
            : folly::AsyncSSLSocket::newSocket(opts.sslContext, &innerEvb);

        const folly::SocketAddress socketAddr(addr, port);
        socket->connect(
            &cb, // notifies reconnecting channel
            socketAddr,
            opts.connectTimeout.count(),
            getSocketOptionMap(opts.maybeIpTos),
            opts.bindAddr);

        if (opts.enableKeepAlive) {
          tryEnableKeepAliveForSocket(socket.get());
        }

        apache::thrift::RocketClientChannel::Ptr ch =
            apache::thrift::RocketClientChannel::newChannel(std::move(socket));
        ch->setTimeout(opts.processingTimeout.count());
        setCompressionTransform(ch.get());
        return ch;
      };
}

} // namespace openr::detail
