/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/OpenrEventBase.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/platform/NetlinkFibHandler.h>

DEFINE_int32(
    fib_thrift_port, 60100, "Thrift server port for the NetlinkFibHandler");

using openr::NetlinkFibHandler;

int
main(int argc, char** argv) {
  // Init everything
  folly::init(&argc, &argv);

  folly::EventBase mainEvb;
  openr::EventBaseStopSignalHandler handler(&mainEvb);
  handler.registerSignalHandler(SIGINT);
  handler.registerSignalHandler(SIGQUIT);
  handler.registerSignalHandler(SIGTERM);

  std::vector<std::thread> allThreads{};

  auto nlEvb = std::make_unique<folly::EventBase>();
  openr::messaging::ReplicateQueue<openr::fbnl::NetlinkEvent>
      netlinkEventsQueue;
  auto nlSock = std::make_unique<openr::fbnl::NetlinkProtocolSocket>(
      nlEvb.get(), netlinkEventsQueue);
  allThreads.emplace_back(std::thread([&nlEvb]() {
    XLOG(INFO) << "Starting NetlinkProtolSocketEvl thread...";
    folly::setThreadName("NetlinkProtolSocketEvl");
    nlEvb->loopForever();
    XLOG(INFO) << "NetlinkProtolSocketEvl thread stopped.";
  }));
  nlEvb->waitUntilRunning();

  apache::thrift::ThriftServer linuxFibAgentServer;
  auto fibHandler = std::make_shared<NetlinkFibHandler>(nlSock.get());

  // start FibService thread
  auto fibThriftThread = std::thread([fibHandler, &linuxFibAgentServer]() {
    folly::setThreadName("FibService");
    linuxFibAgentServer.setNumIOWorkerThreads(1);
    linuxFibAgentServer.setNumCPUWorkerThreads(1);
    linuxFibAgentServer.setPort(FLAGS_fib_thrift_port);
    linuxFibAgentServer.setInterface(fibHandler);
    linuxFibAgentServer.setDuplex(true);

    XLOG(INFO) << "Fib Agent starting...";
    linuxFibAgentServer.serve();
    XLOG(INFO) << "Fib Agent stopped.";
  });
  allThreads.emplace_back(std::move(fibThriftThread));

  XLOG(INFO) << "Main event loop starting...";
  mainEvb.loopForever();
  XLOG(INFO) << "Main event loop stopped.";

  // close queue
  netlinkEventsQueue.close();

  // Stop eventbase
  nlEvb->terminateLoopSoon();

  // Stop thrift server
  linuxFibAgentServer.stop();

  // Wait for threads to finish
  for (auto& t : allThreads) {
    t.join();
  }

  fibHandler.reset();
  nlSock.reset();
  nlEvb.reset();

  return 0;
}
