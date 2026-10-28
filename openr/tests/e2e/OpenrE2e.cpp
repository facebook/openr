/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrE2eInternal.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <folly/io/async/AsyncSocket.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/config/Config.h>
#include <openr/tests/e2e/OpenrE2eConfig.h>
#include <openr/tests/e2e/OpenrModuleGraph.h>
#include <openr/tests/mocks/MockIoProvider.h>

namespace openr::e2e {
namespace {

std::string
requireNodeName(std::string nodeName) {
  if (nodeName.empty()) {
    throw std::invalid_argument("Open/R E2E node name must not be empty");
  }
  return nodeName;
}

} // namespace

OpenrE2eLab::Impl::Impl(std::string nodeName)
    : nodeName_{requireNodeName(std::move(nodeName))},
      platform_{nodeName_, queues_.netlinkEvents},
      ioProvider_{std::make_shared<MockIoProvider>()} {}

OpenrE2eLab::Impl::~Impl() {
  stop();
}

const OpenrE2eLab::Impl::Peer&
OpenrE2eLab::Impl::peer(const std::string& name) const {
  const auto it = peers_.find(name);
  if (it == peers_.end()) {
    throw std::invalid_argument("Unknown Open/R E2E peer: " + name);
  }
  return it->second;
}

void
OpenrE2eLab::Impl::requireStarted() const {
  if (!started_ || stopped_) {
    throw std::logic_error("Open/R E2E lab is not running");
  }
}

void
OpenrE2eLab::Impl::reconnectControlClient() {
  controlClient_.reset();
  auto socket = folly::AsyncSocket::newSocket(
      &controlEventBase_, "::1", graph_->controlPort(), 5000);
  controlClient_ = std::make_unique<E2eCtrlClient>(
      apache::thrift::RocketClientChannel::newChannel(std::move(socket)));
}

OpenrE2eLab::OpenrE2eLab(std::string nodeName)
    : impl_{std::make_unique<Impl>(std::move(nodeName))} {}

OpenrE2eLab::~OpenrE2eLab() = default;

void
OpenrE2eLab::Impl::addPeer(const std::string& name) {
  if (stopped_) {
    throw std::logic_error("Open/R E2E lab has stopped");
  }
  if (name.empty() || name == nodeName_ || peers_.count(name)) {
    throw std::invalid_argument("Open/R E2E peer name must be unique");
  }

  const auto index = peers_.size() + 1;
  Peer entry{
      name,
      "e2e-dut-" + std::to_string(index),
      "e2e-peer-" + std::to_string(index),
      static_cast<int>(100 + 2 * index),
      static_cast<int>(101 + 2 * index),
      "fe80::" + std::to_string(100 + index),
      "fe80::" + std::to_string(200 + index),
      {}};
  entry.synthetic = std::make_unique<SyntheticPeer>(SyntheticPeerConfig{
      nodeName_,
      entry.name,
      entry.dutIfName,
      entry.peerIfName,
      entry.dutIfIndex,
      entry.peerIfIndex,
      entry.dutV6,
      entry.peerV6,
      kvStoreService_.port(),
      ioProvider_});
  const auto& joined = peers_.emplace(name, std::move(entry)).first->second;
  if (!started_) {
    return;
  }

  ioProvider_->addIfNameIfIndex(
      {{joined.dutIfName, joined.dutIfIndex},
       {joined.peerIfName, joined.peerIfIndex}});
  joined.synthetic->start();

  ConnectedIfPairs links;
  for (const auto& [_, p] : peers_) {
    links[p.dutIfName].emplace_back(p.peerIfName, 0);
    links[p.peerIfName].emplace_back(p.dutIfName, 0);
  }
  ioProvider_->setConnectedPairs(std::move(links));

  platform_.setLink(joined.dutIfName, joined.dutIfIndex, true);
  platform_.setAddress(joined.dutIfName, joined.dutV6 + "/64", true);
}

void
OpenrE2eLab::Impl::start() {
  if (started_ || stopped_) {
    throw std::logic_error("Open/R E2E lab cannot be restarted");
  }
  started_ = true;

  try {
    ConnectedIfPairs links;
    IfNameAndifIndex interfaces;
    for (const auto& [_, p] : peers_) {
      links[p.dutIfName].emplace_back(p.peerIfName, 0);
      links[p.peerIfName].emplace_back(p.dutIfName, 0);
      interfaces.emplace_back(p.dutIfName, p.dutIfIndex);
      interfaces.emplace_back(p.peerIfName, p.peerIfIndex);
    }
    ioProvider_->setConnectedPairs(std::move(links));
    ioProvider_->addIfNameIfIndex(interfaces);

    platform_.start();
    for (auto& [_, p] : peers_) {
      p.synthetic->start();
    }
    auto config = std::make_shared<Config>(makeOpenrE2eConfig(
        nodeName_,
        platform_.fibPort(),
        kvStoreService_.port(),
        platform_.persistentStorePath()));
    graph_ = std::make_unique<OpenrModuleGraph>(
        std::move(config),
        queues_,
        platform_,
        kvStoreService_,
        ioProvider_,
        /*manageIoProvider=*/true);
    graph_->start();

    reconnectControlClient();

    for (const auto& [_, p] : peers_) {
      platform_.setLink(p.dutIfName, p.dutIfIndex, true);
      platform_.setAddress(p.dutIfName, p.dutV6 + "/64", true);
    }
  } catch (...) {
    stop();
    throw;
  }
}

void
OpenrE2eLab::Impl::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  controlClient_.reset();
  for (auto& [_, p] : peers_) {
    p.synthetic->stopSpark();
  }
  graph_.reset();
  for (auto& [_, p] : peers_) {
    p.synthetic->stop();
  }
  peers_.clear();
  platform_.stop();
}

void
OpenrE2eLab::Impl::advertisePrefix(
    const std::string& name, const std::string& cidr) {
  requireStarted();
  peer(name).synthetic->advertisePrefix(cidr);
}

void
OpenrE2eLab::Impl::withdrawPrefix(
    const std::string& name, const std::string& cidr) {
  requireStarted();
  peer(name).synthetic->withdrawPrefix(cidr);
}

void
OpenrE2eLab::Impl::advertiseLocalPrefix(const std::string& cidr) {
  requireStarted();
  const std::vector<thrift::PrefixEntry> entries{
      createPrefixEntry(toIpPrefix(folly::IPAddress::createNetwork(cidr)))};
  controlClient_->sync_advertisePrefixes(entries);
}

void
OpenrE2eLab::Impl::withdrawLocalPrefix(const std::string& cidr) {
  requireStarted();
  const std::vector<thrift::PrefixEntry> entries{
      createPrefixEntry(toIpPrefix(folly::IPAddress::createNetwork(cidr)))};
  controlClient_->sync_withdrawPrefixes(entries);
}

void
OpenrE2eLab::Impl::setLinkUp(const std::string& name, bool isUp) {
  requireStarted();
  const auto& p = peer(name);
  platform_.setLink(p.dutIfName, p.dutIfIndex, isUp);
}

void
OpenrE2eLab::Impl::restartFibService() {
  requireStarted();
  if (!platform_.waitForFibServiceKeepaliveCalls(2, std::chrono::seconds{10})) {
    throw std::runtime_error("FibService keepalive did not initialize");
  }
  platform_.restartFibService();
}

void
OpenrE2eLab::Impl::setFibServiceAvailable(bool isAvailable) {
  requireStarted();
  platform_.setFibServiceAvailable(isAvailable);
}

void
OpenrE2eLab::Impl::setLinkMetric(const std::string& name, std::int32_t metric) {
  requireStarted();
  controlClient_->sync_setInterfaceMetric(peer(name).dutIfName, metric);
}

void
OpenrE2eLab::Impl::clearLinkMetric(const std::string& name) {
  requireStarted();
  controlClient_->sync_unsetInterfaceMetric(peer(name).dutIfName);
}

void
OpenrE2eLab::addPeer(const std::string& name) {
  impl_->addPeer(name);
}

void
OpenrE2eLab::start() {
  impl_->start();
}

void
OpenrE2eLab::stop() {
  impl_->stop();
}

void
OpenrE2eLab::advertisePrefix(const std::string& peer, const std::string& cidr) {
  impl_->advertisePrefix(peer, cidr);
}

void
OpenrE2eLab::withdrawPrefix(const std::string& peer, const std::string& cidr) {
  impl_->withdrawPrefix(peer, cidr);
}

void
OpenrE2eLab::advertiseLocalPrefix(const std::string& cidr) {
  impl_->advertiseLocalPrefix(cidr);
}

void
OpenrE2eLab::withdrawLocalPrefix(const std::string& cidr) {
  impl_->withdrawLocalPrefix(cidr);
}

void
OpenrE2eLab::setLinkUp(const std::string& peer, bool isUp) {
  impl_->setLinkUp(peer, isUp);
}

void
OpenrE2eLab::restartFibService() {
  impl_->restartFibService();
}

void
OpenrE2eLab::setFibServiceAvailable(bool isAvailable) {
  impl_->setFibServiceAvailable(isAvailable);
}

void
OpenrE2eLab::setLinkMetric(const std::string& peer, std::int32_t metric) {
  impl_->setLinkMetric(peer, metric);
}

void
OpenrE2eLab::clearLinkMetric(const std::string& peer) {
  impl_->clearLinkMetric(peer);
}

} // namespace openr::e2e
