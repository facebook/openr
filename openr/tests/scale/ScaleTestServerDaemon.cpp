/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <csignal>
#include <memory>

#include <folly/SocketAddress.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/tests/scale/ScaleTestServerHandler.h>

DEFINE_int32(thrift_port, 2019, "Port for the scale-test Thrift server");
DEFINE_bool(loopback_only, true, "Bind to loopback only (lab tool default)");

namespace {

// SIGINT/SIGTERM handlers need a server handle. We can't pass state through
// the C signal-handler ABI, so a file-scope shared_ptr is the least-bad
// option (alternative: folly::Singleton, which is heavier).
// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
std::shared_ptr<apache::thrift::ThriftServer> g_server;

void
shutdownHandler(int /* signal */) {
  if (g_server) {
    g_server->stop();
  }
}

} // namespace

int
main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  std::signal(SIGINT, shutdownHandler);
  std::signal(SIGTERM, shutdownHandler);

  auto handler = std::make_shared<openr::ScaleTestServerHandler>();
  g_server = std::make_shared<apache::thrift::ThriftServer>();
  g_server->setInterface(handler);
  if (FLAGS_loopback_only) {
    g_server->setAddress(folly::SocketAddress("::1", FLAGS_thrift_port));
  } else {
    g_server->setPort(FLAGS_thrift_port);
  }

  XLOGF(INFO, "ScaleTestServer daemon listening on port {}", FLAGS_thrift_port);
  g_server->serve();
  XLOGF(INFO, "ScaleTestServer daemon stopped");
  return 0;
}
