/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fbzmq/async/StopEventLoopSignalHandler.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/platform/NetlinkServiceHandler.h>

DEFINE_int32(
    system_thrift_port, 60099, "Thrift server port for NetlinkServiceHandler");
DEFINE_int32(
    fib_thrift_port, 60100, "Thrift server port for the NetlinkFibHandler");
DEFINE_string(
    chdir, "/tmp", "Change current directory to this after loading config");
DEFINE_string(
    platform_pub_url,
    "ipc://platform-pub-url",
    "Publisher URL for interface/address notifications");
DEFINE_bool(
    enable_netlink_fib_handler,
    true,
    "If set, netlink fib handler will be started for route programming.");
DEFINE_bool(
    enable_netlink_system_handler,
    true,
    "If set, netlink system handler will be started");

using openr::NetlinkServiceHandler;

int
main(int argc, char** argv) {
  // Init everything
  folly::init(&argc, &argv);

  // change directory if chdir specified
  if (!FLAGS_chdir.empty()) {
    ::chdir(FLAGS_chdir.c_str());
  }

  fbzmq::Context context;
  fbzmq::ZmqEventLoop mainEventLoop;

  fbzmq::StopEventLoopSignalHandler eventLoopHandler(&mainEventLoop);
  eventLoopHandler.registerSignalHandler(SIGINT);
  eventLoopHandler.registerSignalHandler(SIGQUIT);
  eventLoopHandler.registerSignalHandler(SIGTERM);

  std::vector<std::thread> allThreads{};

  // Only need to start the NetlinkService on 'system_thrift_port'
  apache::thrift::ThriftServer netlinkServiceServer;
  auto nlHandler = std::make_shared<NetlinkServiceHandler>(
      context,
      openr::PlatformPublisherUrl{FLAGS_platform_pub_url},
      &mainEventLoop);

  auto systemThriftThread =
    std::thread([nlHandler, &netlinkServiceServer]() noexcept {
      folly::setThreadName("SystemService");
      netlinkServiceServer.setNWorkerThreads(1);
      netlinkServiceServer.setNPoolThreads(1);
      netlinkServiceServer.setPort(FLAGS_system_thrift_port);
      netlinkServiceServer.setInterface(nlHandler);

      LOG(INFO) << "Netlink Service starting...";
      netlinkServiceServer.serve();
      LOG(INFO) << "Netlink Service stopped.";
    });
  allThreads.emplace_back(std::move(systemThriftThread));

  LOG(INFO) << "Main event loop starting...";
  mainEventLoop.run();
  LOG(INFO) << "Main event loop stopped.";

  netlinkServiceServer.stop();

  // Wait for threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  return 0;
}
