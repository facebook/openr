/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/platform/NetlinkFibHandler.h>
#include <openr/platform/PlatformPublisher.h>

DEFINE_bool(
    enable_netlink_fib_handler,
    true,
    "If set, netlink fib handler will be started for route programming.");
DEFINE_int32(
    fib_thrift_port, 60100, "Thrift server port for the NetlinkFibHandler");

// [TO BE DEPRECATED]
DEFINE_string(
    platform_pub_url,
    "ipc:///tmp/platform-pub-url",
    "Publisher URL for interface/address notifications");

using openr::NetlinkFibHandler;

int
main(int argc, char** argv) {
  // Init everything
  folly::init(&argc, &argv);

  fbzmq::Context context;
  fbzmq::ZmqEventLoop mainEventLoop;

  fbzmq::StopEventLoopSignalHandler eventLoopHandler(&mainEventLoop);
  eventLoopHandler.registerSignalHandler(SIGINT);
  eventLoopHandler.registerSignalHandler(SIGQUIT);
  eventLoopHandler.registerSignalHandler(SIGTERM);

  std::vector<std::thread> allThreads{};

  auto nlEvb = std::make_unique<folly::EventBase>();
  auto nlSock =
      std::make_unique<openr::fbnl::NetlinkProtocolSocket>(nlEvb.get());
  allThreads.emplace_back(std::thread([&nlEvb]() {
    LOG(INFO) << "Starting NetlinkProtolSocketEvl thread...";
    folly::setThreadName("NetlinkProtolSocketEvl");
    nlEvb->loopForever();
    LOG(INFO) << "NetlinkProtolSocketEvl thread stopped.";
  }));
  nlEvb->waitUntilRunning();

  // Create event publisher to handle event subscription
  auto eventPublisher = std::make_unique<openr::PlatformPublisher>(
      context,
      openr::PlatformPublisherUrl{FLAGS_platform_pub_url},
      nlSock.get());

  apache::thrift::ThriftServer linuxFibAgentServer;
  if (FLAGS_enable_netlink_fib_handler) {
    // start FibService thread
    auto fibHandler = std::make_shared<NetlinkFibHandler>(nlSock.get());

    auto fibThriftThread = std::thread([fibHandler, &linuxFibAgentServer]() {
      folly::setThreadName("FibService");
      linuxFibAgentServer.setNWorkerThreads(1);
      linuxFibAgentServer.setNPoolThreads(1);
      linuxFibAgentServer.setPort(FLAGS_fib_thrift_port);
      linuxFibAgentServer.setInterface(fibHandler);
      linuxFibAgentServer.setDuplex(true);

      LOG(INFO) << "Fib Agent starting...";
      linuxFibAgentServer.serve();
      LOG(INFO) << "Fib Agent stopped.";
    });
    allThreads.emplace_back(std::move(fibThriftThread));
  }

  LOG(INFO) << "Main event loop starting...";
  mainEventLoop.run();
  LOG(INFO) << "Main event loop stopped.";

  nlEvb->terminateLoopSoon();

  if (FLAGS_enable_netlink_fib_handler) {
    linuxFibAgentServer.stop();
  }

  // Wait for threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  if (nlSock) {
    nlSock.reset();
  }

  if (eventPublisher) {
    eventPublisher.reset();
  }

  return 0;
}
