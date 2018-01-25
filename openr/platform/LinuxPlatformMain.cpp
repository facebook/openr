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

#include <openr/platform/NetlinkFibHandler.h>
#include <openr/platform/NetlinkSystemHandler.h>

DEFINE_int32(
    system_thrift_port, 60099, "Thrift server port for NetlinkSystemHandler");
DEFINE_int32(
    fib_thrift_port, 60100, "Thrift server port for the NetlinkFibHandler");
DEFINE_string(
    chdir, "/tmp", "Change current directory to this after loading config");
DEFINE_string(
    platform_pub_url,
    "ipc://platform-pub-url",
    "Publisher URL for interface/address notifications");

using openr::NetlinkFibHandler;
using openr::NetlinkSystemHandler;

int
main(int argc, char** argv) {
  // Init everything
  folly::init(&argc, &argv);

  // change directory if chdir specified
  if (!FLAGS_chdir.empty()) {
    ::chdir(FLAGS_chdir.c_str());
  }

  fbzmq::Context context;
  fbzmq::ZmqEventLoop mainEventLoop, platformEvl;

  fbzmq::StopEventLoopSignalHandler eventLoopHandler(&mainEventLoop);
  eventLoopHandler.registerSignalHandler(SIGINT);
  eventLoopHandler.registerSignalHandler(SIGQUIT);
  eventLoopHandler.registerSignalHandler(SIGTERM);

  auto platformZmqThread = std::thread([&platformEvl]() noexcept {
    LOG(INFO) << "Starting Netlink Fib Platform ZMQ event loop thread ...";
    folly::setThreadName("FibPlatform");
    platformEvl.run();
    LOG(INFO) << "Stopped Netlink Fib Platform ZMQ event loop thread ...";
  });
  platformEvl.waitUntilRunning();

  auto nlHandler = std::make_shared<NetlinkSystemHandler>(
      context,
      openr::PlatformPublisherUrl{FLAGS_platform_pub_url},
      &platformEvl);

  apache::thrift::ThriftServer systemServiceServer;
  auto systemThriftThread =
      std::thread([nlHandler, &systemServiceServer]() noexcept {
        folly::setThreadName("SystemService");
        systemServiceServer.setNWorkerThreads(1);
        systemServiceServer.setNPoolThreads(1);
        systemServiceServer.setPort(FLAGS_system_thrift_port);
        systemServiceServer.setInterface(nlHandler);

        LOG(INFO) << "Starting System Service server...";
        systemServiceServer.serve();
        LOG(INFO) << "Stopped System Service server...";
      });

  auto fibHandler = std::make_shared<NetlinkFibHandler>(&platformEvl);

  apache::thrift::ThriftServer linuxFibAgentServer;
  auto fibThriftThread = std::thread([fibHandler, &linuxFibAgentServer]() {
    folly::setThreadName("FibService");
    linuxFibAgentServer.setNWorkerThreads(1);
    linuxFibAgentServer.setNPoolThreads(1);
    linuxFibAgentServer.setPort(FLAGS_fib_thrift_port);
    linuxFibAgentServer.setInterface(fibHandler);

    LOG(INFO) << "Starting Fib Service server...";
    linuxFibAgentServer.serve();
  });

  LOG(INFO) << "Starting the main loop";
  mainEventLoop.run();
  LOG(INFO) << "Stopping the main loop";

  LOG(INFO) << "Main Event loop stopped... Stopping the servers";
  linuxFibAgentServer.stop();
  systemServiceServer.stop();

  // Wait for threads to finish
  systemThriftThread.join();
  fibThriftThread.join();
  LOG(INFO) << "Main Event loop stopped... Server threads stopped";

  platformEvl.stop();
  platformEvl.waitUntilStopped();
  LOG(INFO) << "Main Event loop stopped... Platform zmq event loops stopped";

  platformZmqThread.join();

  LOG(INFO) << "All threads stopped ..";

  return 0;
}
