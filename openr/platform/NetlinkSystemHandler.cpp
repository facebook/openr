/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkSystemHandler.h"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/BinaryProtocol.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/AddressUtil.h>
#include <openr/nl/NetlinkSubscriber.h>

using namespace fbzmq;
using apache::thrift::FRAGILE;

const std::chrono::seconds kNetlinkDbResyncInterval{20};

namespace openr {
NetlinkSystemHandler::NetlinkSystemHandler(
    fbzmq::Context& context,
    const PlatformPublisherUrl& platformPublisherUrl,
    fbzmq::ZmqEventLoop* zmqEventLoop,
    std::shared_ptr<NetlinkRouteSocket> netlinkRouteSocket)
    : nlImpl_(std::make_unique<NetlinkSystemHandler::NLSubscriberImpl>(
          context, platformPublisherUrl, zmqEventLoop)),
      mainEventLoop_(zmqEventLoop),
      netlinkRouteSocket_(netlinkRouteSocket) {
  initNetlinkSystemHandler();
}

NetlinkSystemHandler::~NetlinkSystemHandler() {}

// Private class for implementing NetlinkSystemHandler internals
class NetlinkSystemHandler::NLSubscriberImpl final
    : public NetlinkSubscriber::Handler {
 public:
  NLSubscriberImpl(
      fbzmq::Context& context,
      const PlatformPublisherUrl& platformPublisherUrl,
      fbzmq::ZmqEventLoop* zmqEventLoop)
      : evl_(zmqEventLoop) {
    CHECK(evl_) << "Invalid ZMQ Event loop handle";

    platformPublisher_ =
        std::make_unique<PlatformPublisher>(context, platformPublisherUrl);
  }

  ~NLSubscriberImpl() override = default;

  // non-copyable object
  NLSubscriberImpl(const NLSubscriberImpl&) = delete;
  NLSubscriberImpl& operator=(const NLSubscriberImpl&) = delete;

  folly::Future<folly::Unit> initNL();

  folly::Future<std::unique_ptr<std::vector<thrift::Link>>> getAllLinks();

  folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
  getAllNeighbors();

 private:
  void doInitNL();

  // Called at init and then at periodic re-sync as a protection
  // against missed netlink events
  void updateNetlinkDb();

  std::unique_ptr<std::vector<thrift::Link>> doGetAllLinks();

  std::unique_ptr<std::vector<thrift::NeighborEntry>> doGetAllNeighbors();

  // Override method for NetlinkSubscriber link/address/neighbor events
  void linkEventFunc(const LinkEntry& linkEntry) override;
  void addrEventFunc(const AddrEntry& addrEntry) override;
  void neighborEventFunc(const NeighborEntry& neighborEntry) override;

  fbzmq::ZmqEventLoop* evl_{nullptr};

  // Interface/nextHop-IP => MacAddress mapping
  Neighbors neighborDb_{};

  // Interface/link name => link attributes mapping
  Links linkDb_{};

  // Periodic timer to resync NetlinkDb
  std::unique_ptr<fbzmq::ZmqTimeout> netlinkDbResyncTimer_;

  // Used to get neighbor entries from kernel
  std::unique_ptr<NetlinkSubscriber> netlinkSubscriber_;

  // Used to publish Netlink event
  std::unique_ptr<PlatformPublisher> platformPublisher_;
};

folly::Future<folly::Unit>
NetlinkSystemHandler::NLSubscriberImpl::initNL() {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  LOG(INFO) << "Initializng Netlink from server thread";
  evl_->runInEventLoop([this, promise = std::move(promise)]() mutable {
    try {
      doInitNL();
      promise.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error in Netlink init";
      promise.setException(ex);
    }
  });
  return future;
}

folly::Future<std::unique_ptr<std::vector<thrift::Link>>>
NetlinkSystemHandler::NLSubscriberImpl::getAllLinks() {
  folly::Promise<std::unique_ptr<std::vector<thrift::Link>>> promise;
  auto future = promise.getFuture();

  VLOG(3) << "Requesting Link Db";

  evl_->runInEventLoop([this, promise = std::move(promise)]() mutable {
    try {
      auto linkDb = doGetAllLinks();
      promise.setValue(std::move(linkDb));
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error in getting Link Db from Netlink";
      promise.setException(ex);
    }
  });

  return future;
}

folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
NetlinkSystemHandler::NLSubscriberImpl::getAllNeighbors() {
  folly::Promise<std::unique_ptr<std::vector<thrift::NeighborEntry>>> promise;
  auto future = promise.getFuture();

  VLOG(3) << "Requesting Neighbor Db";

  // pass params by copy
  evl_->runInEventLoop([this, promise = std::move(promise)]() mutable {
    try {
      auto neighborDb = doGetAllNeighbors();
      promise.setValue(std::move(neighborDb));
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error in getting Neighbor Db from Netlink";
      promise.setException(ex);
    }
  });

  return future;
}

void
NetlinkSystemHandler::NLSubscriberImpl::doInitNL() {
  VLOG(2) << "Performing NL Init";

  // We are the subscriber
  // We need to create this here because we must be in zmqEventLoop for this
  netlinkSubscriber_ = std::make_unique<NetlinkSubscriber>(evl_, this);

  // Periodic re-sync of neighbor entries from netlink
  netlinkDbResyncTimer_ = fbzmq::ZmqTimeout::make(evl_, [this]() noexcept {
    VLOG(2) << "Re-syncing Netlink DB";
    updateNetlinkDb();
    VLOG(2) << "Completed re-syncing Netlink DB from Netlink Subscriber";
  });
  netlinkDbResyncTimer_->scheduleTimeout(
      kNetlinkDbResyncInterval, true /* is Periodic */);

  updateNetlinkDb();
}

std::unique_ptr<std::vector<thrift::Link>>
NetlinkSystemHandler::NLSubscriberImpl::doGetAllLinks() {
  updateNetlinkDb();
  auto linkDb = std::make_unique<std::vector<thrift::Link>>();

  for (const auto link : linkDb_) {
    thrift::Link linkEntry;
    linkEntry.ifName = link.first;
    linkEntry.ifIndex = link.second.ifIndex;
    linkEntry.isUp = link.second.isUp;
    for (const auto network : link.second.networks) {
      linkEntry.networks.push_back(thrift::IpPrefix(
          FRAGILE, toBinaryAddress(network.first), network.second));
    }
    linkDb->push_back(linkEntry);
  }
  return linkDb;
}

std::unique_ptr<std::vector<thrift::NeighborEntry>>
NetlinkSystemHandler::NLSubscriberImpl::doGetAllNeighbors() {
  updateNetlinkDb();
  auto neighborDb = std::make_unique<std::vector<thrift::NeighborEntry>>();

  for (const auto kv : neighborDb_) {
    thrift::NeighborEntry neighborEntry = thrift::NeighborEntry(
        FRAGILE,
        kv.first.first,
        toBinaryAddress(kv.first.second),
        kv.second.toString(),
        true);
    neighborDb->push_back(neighborEntry);
  }
  return neighborDb;
}

void
NetlinkSystemHandler::NLSubscriberImpl::updateNetlinkDb() {
  VLOG(3) << "Updating neighborDb via netlink";
  neighborDb_ = netlinkSubscriber_->getAllReachableNeighbors();
  VLOG(3) << "Updating linkDb via netlink";
  linkDb_ = netlinkSubscriber_->getAllLinks();
}

void
NetlinkSystemHandler::NLSubscriberImpl::linkEventFunc(
    const LinkEntry& linkEntry) {
  VLOG(4) << "Handling Link Event in NetlinkSystemHandler...";
  platformPublisher_->publishLinkEvent(thrift::LinkEntry(
      FRAGILE,
      linkEntry.ifName,
      linkEntry.ifIndex,
      linkEntry.isUp,
      Constants::kDefaultAdjWeight));
}

void
NetlinkSystemHandler::NLSubscriberImpl::addrEventFunc(
    const AddrEntry& addrEntry) {
  VLOG(4) << "Handling Address Event in NetlinkSystemHandler...";
  platformPublisher_->publishAddrEvent(thrift::AddrEntry(
      FRAGILE,
      addrEntry.ifName,
      thrift::IpPrefix(
          FRAGILE,
          toBinaryAddress(addrEntry.network.first),
          addrEntry.network.second),
      addrEntry.isValid));
}

void
NetlinkSystemHandler::NLSubscriberImpl::neighborEventFunc(
    const NeighborEntry& neighborEntry) {
  VLOG(4) << "Handling Neighbor Event in NetlinkSystemHandler...";
  platformPublisher_->publishNeighborEvent(thrift::NeighborEntry(
      FRAGILE,
      neighborEntry.ifName,
      toBinaryAddress(neighborEntry.destination),
      neighborEntry.linkAddress.toString(),
      neighborEntry.isReachable));
}

void
NetlinkSystemHandler::initNetlinkSystemHandler() {
  VLOG(3) << "Building NL Db from existing Netlink cache";

  nlImpl_->initNL();
}

folly::Future<std::unique_ptr<std::vector<thrift::Link>>>
NetlinkSystemHandler::future_getAllLinks() {
  VLOG(3) << "Query links from Netlink according to link name";

  auto future = nlImpl_->getAllLinks();
  return future;
}

folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
NetlinkSystemHandler::future_getAllNeighbors() {
  VLOG(3) << "Query all reachable neighbors from Netlink";

  auto future = nlImpl_->getAllNeighbors();
  return future;
}

folly::Future<folly::Unit> NetlinkSystemHandler::future_addIfaceAddresses(
  std::unique_ptr<std::string> iface,
  std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Add iface addresses";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     addresses = std::move(addrs),
     ifName = std::move(iface)]() mutable {
      try {
        for (const auto& addr : *addresses) {
          const auto& prefix = toIPNetwork(addr);
          doAddIfaceAddr(*ifName, prefix);
        }
        p.setValue();
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
    return future;
}

void NetlinkSystemHandler::doAddIfaceAddr(
  const std::string& ifName,
  const folly::CIDRNetwork& prefix) {
  int ifIndex = netlinkRouteSocket_->getIfIndex(ifName).get();
  fbnl::IfAddressBuilder builder;
  auto addr = builder.setPrefix(prefix)
                     .setIfIndex(ifIndex)
                     .build();
  netlinkRouteSocket_->addIfAddress(std::move(addr)).get();
}

folly::Future<folly::Unit> NetlinkSystemHandler::future_removeIfaceAddresses(
  std::unique_ptr<std::string> iface,
  std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Remove iface addresses";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     addresses = std::move(addrs),
     ifName = std::move(iface)]() mutable {
      try {
        for (const auto& addr : *addresses) {
          const auto& prefix = toIPNetwork(addr);
          doRemoveIfaceAddr(*ifName, prefix);
        }
        p.setValue();
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
    return future;
}

folly::Future<std::unique_ptr<std::vector<::openr::thrift::IpPrefix>>>
NetlinkSystemHandler::future_getIfaceAddresses(
  std::unique_ptr<std::string> iface, int16_t family, int16_t scope) {
  VLOG(3) << "Get iface addresses";
  folly::Promise<std::unique_ptr<
                  std::vector<::openr::thrift::IpPrefix>>> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     ifName = std::move(iface), family, scope] () mutable {
       try {
         auto addrs = doGetIfaceAddrs(*ifName, family, scope);
         p.setValue(std::move(addrs));
       } catch (const std::exception& ex) {
         p.setException(ex);
       }
     });
  return future;
}

std::unique_ptr<std::vector<openr::thrift::IpPrefix>>
NetlinkSystemHandler::doGetIfaceAddrs(
  const std::string& iface,
  int16_t family,
  int16_t scope) {
    auto addrs = std::make_unique<std::vector<openr::thrift::IpPrefix>>();
    int ifIndex = netlinkRouteSocket_->getIfIndex(iface).get();
    auto ifAddrs =
      netlinkRouteSocket_->getIfAddrs(ifIndex, family, scope).get();

    addrs->clear();
    for (const auto& ifAddr : ifAddrs) {
      addrs->emplace_back(toIpPrefix(ifAddr.getPrefix().value()));
    }
    return addrs;
}


void NetlinkSystemHandler::doRemoveIfaceAddr(
  const std::string& ifName,
  const folly::CIDRNetwork& prefix) {
  int ifIndex = netlinkRouteSocket_->getIfIndex(ifName).get();
  fbnl::IfAddressBuilder builder;
  builder.setPrefix(prefix)
         .setIfIndex(ifIndex);
  netlinkRouteSocket_->delIfAddress(builder.build()).get();
}

folly::Future<folly::Unit> NetlinkSystemHandler::future_syncIfaceAddresses(
  std::unique_ptr<std::string> iface,
  int16_t family, int16_t scope,
  std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Sync iface addresses";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     ifName = std::move(iface),
     addresses = std::move(addrs),
     family, scope]() mutable {
      try {
        doSyncIfaceAddrs(*ifName, family, scope, *addresses);
        p.setValue();
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
    return future;
}

void NetlinkSystemHandler::doSyncIfaceAddrs(
  const std::string& ifName,
  int16_t family,
  int16_t scope,
  const std::vector<::openr::thrift::IpPrefix>& addrs) {
  int ifIndex = netlinkRouteSocket_->getIfIndex(ifName).get();
  std::vector<fbnl::IfAddress> ifAddrs;
  fbnl::IfAddressBuilder builder;
  for (const auto& addr : addrs) {
    builder.setFamily(family)
           .setIfIndex(ifIndex)
           .setScope(scope)
           .setPrefix(toIPNetwork(addr));
    ifAddrs.emplace_back(builder.build());
    builder.reset();
  }
  netlinkRouteSocket_->
    syncIfAddress(ifIndex, std::move(ifAddrs), family, scope).get();
}

} // namespace openr
