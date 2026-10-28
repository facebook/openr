/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/TestPlatform.h>

#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>

#include <openr/if/gen-cpp2/Platform_types.h>

namespace openr::e2e {

TestPlatform::TestPlatform(
    std::string nodeName,
    messaging::ReplicateQueue<fbnl::NetlinkEvent>& netlinkEvents)
    : persistentStorePath_{(stateDirectory_.path() /
                            fmt::format("{}.bin", nodeName))
                               .string()},
      netlinkSocket_{std::make_unique<fbnl::MockNetlinkProtocolSocket>(
          &netlinkEventBase_, netlinkEvents)},
      netlinkEvents_{
          std::make_unique<NetlinkEventsInjector>(netlinkSocket_.get())},
      fibHandler_{std::make_shared<MockNetlinkFibHandler>()} {
  fibSocket_ =
      folly::AsyncServerSocket::UniquePtr{new folly::AsyncServerSocket()};
  fibSocket_->setReusePortEnabled(true);
  fibSocket_->bind(std::vector<folly::IPAddress>{folly::IPAddress{"::1"}}, 0);
  folly::SocketAddress address;
  fibSocket_->getAddress(&address);
  fibPort_ = address.getPort();
}

TestPlatform::~TestPlatform() {
  stop();
}

void
TestPlatform::start() {
  if (running_) {
    return;
  }
  if (started_) {
    throw std::logic_error("TestPlatform cannot be restarted");
  }
  started_ = true;
  fibServer_ = std::make_shared<apache::thrift::ThriftServer>();
  fibServer_->setNumIOWorkerThreads(1);
  fibServer_->setNumAcceptThreads(1);
  fibServer_->setQueueTimeout(std::chrono::milliseconds::zero());
  fibServer_->setInterface(fibHandler_);
  fibServer_->useExistingSocket(std::move(fibSocket_));
  fibServerThread_ =
      std::make_unique<apache::thrift::util::ScopedServerThread>(fibServer_);
  running_ = true;
}

void
TestPlatform::stop() {
  if (fibServerThread_) {
    fibServerThread_->stop();
    fibServerThread_->join();
    fibServerThread_.reset();
  }
  fibServer_.reset();
  if (running_) {
    fibHandler_->stop();
    running_ = false;
  }
}

uint16_t
TestPlatform::fibPort() const {
  if (!running_) {
    throw std::logic_error("FibService is not running");
  }
  return fibPort_;
}

const std::string&
TestPlatform::persistentStorePath() const {
  return persistentStorePath_;
}

fbnl::MockNetlinkProtocolSocket&
TestPlatform::netlinkSocket() {
  return *netlinkSocket_;
}

MockNetlinkFibHandler&
TestPlatform::fibHandler() {
  return *fibHandler_;
}

void
TestPlatform::setLink(
    const std::string& interfaceName, int interfaceIndex, bool isUp) {
  netlinkEvents_->sendLinkEvent(interfaceName, interfaceIndex, isUp);
}

void
TestPlatform::setAddress(
    const std::string& interfaceName,
    const std::string& prefix,
    bool isPresent) {
  netlinkEvents_->sendAddrEvent(interfaceName, prefix, isPresent);
}

std::vector<thrift::UnicastRoute>
TestPlatform::fibRoutes() const {
  std::vector<thrift::UnicastRoute> routes;
  fibHandler_->getRouteTableByClient(
      routes, static_cast<int16_t>(thrift::FibClient::OPENR));
  return routes;
}

TestPlatform::FibUpdateCounts
TestPlatform::fibUpdateCounts() const {
  return FibUpdateCounts{
      fibHandler_->getFibSyncCount(),
      fibHandler_->getAddRoutesCount(),
      fibHandler_->getDelRoutesCount()};
}

} // namespace openr::e2e
