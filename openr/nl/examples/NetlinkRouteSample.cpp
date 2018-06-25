/**
 * Copyright 20__-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the license found in the
 * LICENSE-examples file in the root directory of this source tree.
 */

#include <map>
#include <memory>
#include <string>
#include <thread>

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Exception.h>
#include <folly/Format.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>
#include <folly/gen/String.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <openr/nl/NetlinkRouteSocket.h>

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;
using folly::gen::unsplit;

using namespace openr;

namespace {
// too bad 0xFB (251) is taken by gated/ospfase
// We will use this as the proto for our routes
const uint8_t kAqRouteProtoId = 99;
} // namespace

void
RouteDumper(fbzmq::ZmqEventLoop* evl) {
  LOG(INFO) << "Starting Route dumper..";

  // Create the netlinkRouteSocket object
  NetlinkRouteSocket netlinkRouteSocket(evl);

  LOG(INFO) << "Dumping Routes";
  LOG(INFO) << "==============";
  auto routes = netlinkRouteSocket.getCachedUnicastRoutes(kAqRouteProtoId).get();

  for (const auto& kv : routes) {
    auto nhopStr =
        from(kv.second) |
        mapped([](const std::pair<std::string, folly::IPAddress>& nextHop) {
          return (std::get<1>(nextHop).str() + "@" + std::get<0>(nextHop));
        }) |
        unsplit<std::string>(", ");

    LOG(INFO) << "Route : " << folly::IPAddress::networkToString(kv.first)
              << " via " << nhopStr;
  }
  LOG(INFO) << "==============";
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  fbzmq::ZmqEventLoop mainEventLoop, fibEvl;

  fbzmq::StopEventLoopSignalHandler eventLoopHandler(&mainEventLoop);
  eventLoopHandler.registerSignalHandler(SIGINT);
  eventLoopHandler.registerSignalHandler(SIGQUIT);
  eventLoopHandler.registerSignalHandler(SIGTERM);

  auto fibZmqThread = std::thread([&fibEvl]() noexcept {
    LOG(INFO) << "Starting Netlink Fib Platform ZMQ event loop thread ...";
    fibEvl.run();
    LOG(INFO) << "Stopped Netlink Fib Platform ZMQ event loop thread ...";
  });
  fibEvl.waitUntilRunning();

  LOG(INFO) << "Starting the main loop";
  mainEventLoop.run();
  LOG(INFO) << "Stopping the main loop";

  RouteDumper(&fibEvl);

  fibEvl.stop();
  fibEvl.waitUntilStopped();
  LOG(INFO) << "Main Event loop stopped... System/Fib zmq event loops stopped";

  fibZmqThread.join();

  return 0;
}
