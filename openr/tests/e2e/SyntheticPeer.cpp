/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/SyntheticPeer.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/scale/FakeKvStoreHandler.h>
#include <openr/tests/scale/SparkFaker.h>

namespace openr::e2e {
namespace {

constexpr char kArea[] = "0";
constexpr char kLoopback[] = "::1";
constexpr uint32_t kRpcTimeoutMs{5000};

SyntheticPeerConfig
validate(SyntheticPeerConfig config) {
  if (config.nodeName.empty() || config.peerName.empty() ||
      config.nodeName == config.peerName || config.dutIfName.empty() ||
      config.peerIfName.empty() || config.dutIfIndex <= 0 ||
      config.peerIfIndex <= 0 || config.dutV6Address.empty() ||
      config.peerV6Address.empty() || config.dutKvStorePort == 0 ||
      !config.ioProvider) {
    throw std::invalid_argument(
        "Synthetic peer needs a real node, link, and KvStore port");
  }
  return config;
}

thrift::KeyVals
makeInitialKeys(const SyntheticPeerConfig& config) {
  const auto adjacency = createThriftAdjacency(
      config.nodeName,
      config.peerIfName,
      config.dutV6Address,
      "0.0.0.0",
      /*metric=*/1,
      /*adjLabel=*/10000 + config.peerIfIndex,
      /*isOverloaded=*/false,
      /*rtt=*/0,
      /*timestamp=*/0,
      /*weight=*/1,
      config.dutIfName);
  auto database = createAdjDb(
      config.peerName,
      {adjacency},
      /*nodeLabel=*/50000 + config.peerIfIndex,
      /*overLoadBit=*/false,
      kArea);
  apache::thrift::CompactSerializer serializer;
  thrift::KeyVals keys;
  keys.emplace(
      "adj:" + config.peerName,
      createThriftValue(
          /*version=*/1,
          config.peerName,
          writeThriftObjStr(database, serializer)));
  return keys;
}

} // namespace

class SyntheticPeer::Impl final {
 public:
  explicit Impl(SyntheticPeerConfig config)
      : config_{validate(std::move(config))},
        handler_{std::make_shared<FakeKvStoreHandler>(
            config_.peerName, makeInitialKeys(config_), kArea)},
        sparkFaker_{config_.ioProvider} {
    kvStoreSocket_ =
        folly::AsyncServerSocket::UniquePtr{new folly::AsyncServerSocket()};
    kvStoreSocket_->setEnableReuseAddr(false);
    kvStoreSocket_->bind(
        std::vector<folly::IPAddress>{folly::IPAddress{kLoopback}}, 0);
    folly::SocketAddress address;
    kvStoreSocket_->getAddress(&address);
    kvStorePort_ = address.getPort();

    sparkFaker_.addNeighbor(
        config_.peerName,
        config_.peerIfName,
        config_.peerIfIndex,
        config_.peerV6Address,
        config_.dutIfName,
        config_.dutIfIndex);
    if (!sparkFaker_.setNeighborArea(config_.peerName, kArea) ||
        !sparkFaker_.setNeighborCtrlPort(config_.peerName, kvStorePort_)) {
      throw std::logic_error("Spark peer was not registered");
    }
  }

  ~Impl() {
    stop();
  }

  void
  start() {
    if (started_ || stopped_) {
      throw std::logic_error("Synthetic peer cannot be restarted");
    }
    try {
      kvStoreServer_ = std::make_shared<apache::thrift::ThriftServer>();
      kvStoreServer_->setInterface(handler_);
      kvStoreServer_->setQueueTimeout(std::chrono::milliseconds::zero());
      kvStoreServer_->useExistingSocket(std::move(kvStoreSocket_));
      kvStoreThread_ =
          std::make_unique<apache::thrift::ScopedServerInterfaceThread>(
              kvStoreServer_);
      sparkFaker_.start();
      started_ = true;
    } catch (...) {
      stop();
      throw;
    }
  }

  void
  stop() {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    sparkFaker_.stop();
    kvStoreThread_.reset();
    kvStoreServer_.reset();
  }

  void
  publishPrefix(const std::string& cidr, bool withdraw) {
    if (!started_ || stopped_) {
      throw std::logic_error("Synthetic peer is not running");
    }
    const auto prefix = folly::IPAddress::createNetwork(cidr);
    const auto prefixText = folly::IPAddress::networkToString(prefix);
    const auto found = prefixVersions_.find(prefixText);
    if (withdraw && found == prefixVersions_.end()) {
      throw std::invalid_argument("Cannot withdraw an unadvertised prefix");
    }
    const auto version = ++prefixVersions_[prefixText];
    const auto entry = createPrefixEntry(toIpPrefix(prefix));
    auto [key, value] =
        createPrefixKeyValue(config_.peerName, version, entry, kArea, withdraw);
    handler_->updateKey(key, value);

    thrift::KeySetParams params;
    params.keyVals() = thrift::KeyVals{{std::move(key), std::move(value)}};
    params.nodeIds() = std::vector<std::string>{config_.peerName};
    params.senderId() = config_.peerName;

    folly::EventBase eventBase;
    auto socket = folly::AsyncSocket::newSocket(
        &eventBase, kLoopback, config_.dutKvStorePort, kRpcTimeoutMs);
    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(socket));
    channel->setTimeout(kRpcTimeoutMs);
    apache::thrift::Client<thrift::KvStoreService> client{std::move(channel)};
    client.sync_setKvStoreKeyVals(params, kArea);
  }

  const SyntheticPeerConfig config_;
  const std::shared_ptr<FakeKvStoreHandler> handler_;
  SparkFaker sparkFaker_;
  folly::AsyncServerSocket::UniquePtr kvStoreSocket_;
  uint16_t kvStorePort_{0};
  std::shared_ptr<apache::thrift::ThriftServer> kvStoreServer_;
  std::unique_ptr<apache::thrift::ScopedServerInterfaceThread> kvStoreThread_;
  std::map<std::string, int64_t> prefixVersions_;
  bool started_{false};
  bool stopped_{false};
};

SyntheticPeer::SyntheticPeer(SyntheticPeerConfig config)
    : impl_{std::make_unique<Impl>(std::move(config))} {}

SyntheticPeer::~SyntheticPeer() = default;

void
SyntheticPeer::start() {
  impl_->start();
}

void
SyntheticPeer::stop() {
  impl_->stop();
}

void
SyntheticPeer::advertisePrefix(const std::string& cidr) {
  impl_->publishPrefix(cidr, false);
}

void
SyntheticPeer::withdrawPrefix(const std::string& cidr) {
  impl_->publishPrefix(cidr, true);
}

const std::string&
SyntheticPeer::dutIfName() const {
  return impl_->config_.dutIfName;
}

const std::string&
SyntheticPeer::peerV6Address() const {
  return impl_->config_.peerV6Address;
}

uint16_t
SyntheticPeer::kvStorePort() const {
  return impl_->kvStorePort_;
}

} // namespace openr::e2e
