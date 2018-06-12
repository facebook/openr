/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkServiceHandler.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <folly/gen/Core.h>

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

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {
namespace {
  const uint8_t kAqRouteProtoId = 99;
  const std::chrono::seconds kRoutesHoldTimeout{30};
  const std::chrono::seconds kNetlinkDbResyncInterval{20};

  // convert a routeDb into thrift exportable route spec
  std::vector<thrift::UnicastRoute>
  makeRoutes(const std::unordered_map<
             folly::CIDRNetwork,
             std::unordered_set<std::pair<std::string, folly::IPAddress>>>&
                 routeDb) {
    std::vector<thrift::UnicastRoute> routes;

    for (auto const& kv : routeDb) {
      auto const& prefix = kv.first;
      auto const& nextHops = kv.second;

      auto binaryNextHops = from(nextHops) |
          mapped([](const std::pair<std::string, folly::IPAddress>& nextHop) {
                              VLOG(2)
                                  << "mapping next-hop " << nextHop.second.str()
                                  << " dev " << nextHop.first;
                              auto binaryAddr = toBinaryAddress(nextHop.second);
                              if (not nextHop.first.empty()) {
                                binaryAddr.ifName = nextHop.first;
                              }
                              return binaryAddr;
                            }) |
          as<std::vector>();

      routes.emplace_back(thrift::UnicastRoute(
          apache::thrift::FRAGILE,
          thrift::IpPrefix(
              apache::thrift::FRAGILE,
              toBinaryAddress(prefix.first),
              static_cast<int16_t>(prefix.second)),
          std::move(binaryNextHops)));
    }
    return routes;
  }

  std::string
  getClientName(const int16_t clientId) {
    auto it = thrift::_FibClient_VALUES_TO_NAMES.find(
        static_cast<thrift::FibClient>(clientId));
    if (it == thrift::_FibClient_VALUES_TO_NAMES.end()) {
      return folly::sformat("UNKNOWN(id={})", clientId);
    }
    return it->second;
  }

  NextHops
  fromThriftNexthops(const std::vector<thrift::BinaryAddress>& thriftNexthops) {
    NextHops nexthops;
    for (auto const& nexthop : thriftNexthops) {
      nexthops.emplace(
        nexthop.ifName.hasValue() ? nexthop.ifName.value() : "",
        toIPAddress(nexthop)
      );
    }
    return nexthops;
  }

  } // namespace

NetlinkServiceHandler::NetlinkServiceHandler(
    fbzmq::Context& context,
    const PlatformPublisherUrl& platformPublisherUrl,
    fbzmq::ZmqEventLoop* zmqEventLoop)
    : netlinkSocket_(
        std::make_unique<NetlinkRouteSocket>(zmqEventLoop, kAqRouteProtoId)),
      startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count()),
      fibEvl_(zmqEventLoop),
      nlImpl_(std::make_unique<NLSubscriberImpl>(
                  context, platformPublisherUrl, zmqEventLoop)) {
    initHandler();
    LOG(INFO) << "Initialize NetlinkServiceHandler by same event loop";
}

 NetlinkServiceHandler::NetlinkServiceHandler(
    fbzmq::Context& context,
    const PlatformPublisherUrl& platformPublisherUrl,
    fbzmq::ZmqEventLoop* fibEventLoop,
    fbzmq::ZmqEventLoop* systemEventLoop)
    : netlinkSocket_(
        std::make_unique<NetlinkRouteSocket>(fibEventLoop, kAqRouteProtoId)),
      startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count()),
      fibEvl_(fibEventLoop),
      nlImpl_(std::make_unique<NLSubscriberImpl>(
          context, platformPublisherUrl, systemEventLoop)) {
    initHandler();
    LOG(INFO) << "Initialize NetlinkServiceHandler by different event loop";
}

NetlinkServiceHandler::~NetlinkServiceHandler() {}

// Private class for implementing NetlinkSystemHandler internals
class NetlinkServiceHandler::NLSubscriberImpl final
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
NetlinkServiceHandler::NLSubscriberImpl::initNL() {
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
NetlinkServiceHandler::NLSubscriberImpl::getAllLinks() {
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
NetlinkServiceHandler::NLSubscriberImpl::getAllNeighbors() {
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
NetlinkServiceHandler::NLSubscriberImpl::doInitNL() {
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
NetlinkServiceHandler::NLSubscriberImpl::doGetAllLinks() {
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
NetlinkServiceHandler::NLSubscriberImpl::doGetAllNeighbors() {
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
NetlinkServiceHandler::NLSubscriberImpl::updateNetlinkDb() {
  VLOG(3) << "Updating neighborDb via netlink";
  neighborDb_ = netlinkSubscriber_->getAllReachableNeighbors();
  VLOG(3) << "Updating linkDb via netlink";
  linkDb_ = netlinkSubscriber_->getAllLinks();
}

void
NetlinkServiceHandler::NLSubscriberImpl::linkEventFunc(
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
NetlinkServiceHandler::NLSubscriberImpl::addrEventFunc(
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
NetlinkServiceHandler::NLSubscriberImpl::neighborEventFunc(
    const NeighborEntry& neighborEntry) {
  VLOG(4) << "Handling Neighbor Event in NetlinkSystemHandler...";
  platformPublisher_->publishNeighborEvent(thrift::NeighborEntry(
      FRAGILE,
      neighborEntry.ifName,
      toBinaryAddress(neighborEntry.destination),
      neighborEntry.linkAddress.toString(),
      neighborEntry.isReachable));
}

folly::Future<std::unique_ptr<std::vector<thrift::Link>>>
NetlinkServiceHandler::future_getAllLinks() {
  VLOG(3) << "Query links from Netlink according to link name";

  auto future = nlImpl_->getAllLinks();
  return future;
}

folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
NetlinkServiceHandler::future_getAllNeighbors() {
  VLOG(3) << "Query all reachable neighbors from Netlink";

  auto future = nlImpl_->getAllNeighbors();
  return future;
}

void NetlinkServiceHandler::initHandler() {
  CHECK_NOTNULL(fibEvl_);

  keepAliveCheckTimer_ = fbzmq::ZmqTimeout::make(fibEvl_, [&]() noexcept {
    auto now = std::chrono::steady_clock::now();
    if (now - recentKeepAliveTs_ > kRoutesHoldTimeout) {
      LOG(WARNING) << "Open/R health check: FAIL. Expiring routes!";
      auto emptyRoutes = std::make_unique<std::vector<thrift::UnicastRoute>>();
      auto ret = future_syncFib(0 /* clientId */, std::move(emptyRoutes));
      ret.then([]() {
        LOG(WARNING) << "Expired routes on health check failure!";
      });
    } else {
      VLOG(2) << "Open/R health check: PASS";
    }
  });
  fibEvl_->runInEventLoop([&]() {
    const bool isPeriodic = true;
    keepAliveCheckTimer_->scheduleTimeout(kRoutesHoldTimeout, isPeriodic);
  });

  VLOG(3) << "Building NL Db from existing Netlink cache";
  nlImpl_->initNL();
}

folly::Future<folly::Unit>
NetlinkServiceHandler::future_addUnicastRoute(
    int16_t, std::unique_ptr<thrift::UnicastRoute> route) {
  DCHECK(route->nexthops.size());

  VLOG(1) << "Adding/Updating route for " << toString(route->dest);

  auto prefix = toIPNetwork(route->dest);
  auto nexthops = fromThriftNexthops(route->nexthops);
  return netlinkSocket_->addUnicastRoute(
      std::move(prefix),
      std::move(nexthops)
  );
}

folly::Future<folly::Unit>
NetlinkServiceHandler::future_deleteUnicastRoute(
    int16_t, std::unique_ptr<thrift::IpPrefix> prefix) {
  VLOG(1) << "Deleting route for " << toString(*prefix);

  auto myPrefix = toIPNetwork(*prefix);
  return netlinkSocket_->deleteUnicastRoute(myPrefix);
}

folly::Future<folly::Unit>
NetlinkServiceHandler::future_addUnicastRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Adding/Updates routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  // Run all route updates in a single eventloop
  fibEvl_->runImmediatelyOrInEventLoop(
    [ this, clientId,
      promise = std::move(promise),
      routes = std::move(routes)
    ]() mutable {
      for (auto& route : *routes) {
        auto ptr = folly::make_unique<thrift::UnicastRoute>(std::move(route));
        try {
          // This is going to be synchronous call as we are invoking from
          // within event loop
          future_addUnicastRoute(clientId, std::move(ptr)).get();
        } catch (std::exception const& e) {
          promise.setException(e);
          return;
        }
      }
      promise.setValue();
    });

  return future;
}

folly::Future<folly::Unit>
NetlinkServiceHandler::future_deleteUnicastRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) {
  LOG(INFO) << "Deleting routes of client: " << getClientName(clientId);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  fibEvl_->runImmediatelyOrInEventLoop(
    [ this, clientId,
      promise = std::move(promise),
      prefixes = std::move(prefixes)
    ]() mutable {
      for (auto& prefix : *prefixes) {
        auto ptr = folly::make_unique<thrift::IpPrefix>(std::move(prefix));
        try {
          future_deleteUnicastRoute(clientId, std::move(ptr)).get();
        } catch (std::exception const& e) {
          promise.setException(e);
          return;
        }
      }
      promise.setValue();
    });

  return future;
}

folly::Future<folly::Unit>
NetlinkServiceHandler::future_syncFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  LOG(INFO) << "Syncing FIB with provided routes. Client: "
            << getClientName(clientId);

  // Build new routeDb
  UnicastRoutes newRouteDb;
  for (auto const& route : *routes) {
    CHECK(route.nexthops.size());
    auto prefix = toIPNetwork(route.dest);
    auto nexthops = fromThriftNexthops(route.nexthops);
    newRouteDb.emplace(std::move(prefix), std::move(nexthops));
  }

  return netlinkSocket_->syncUnicastRoutes(std::move(newRouteDb));
}

folly::Future<int64_t>
NetlinkServiceHandler::future_periodicKeepAlive(int16_t /* clientId */) {
  VLOG(3) << "Received KeepAlive from OpenR";
  recentKeepAliveTs_ = std::chrono::steady_clock::now();
  return folly::makeFuture(keepAliveId_++);
}

int64_t
NetlinkServiceHandler::aliveSince() {
  VLOG(3) << "Received KeepAlive from OpenR";
  recentKeepAliveTs_ = std::chrono::steady_clock::now();
  return startTime_;
}

openr::thrift::ServiceStatus
NetlinkServiceHandler::getStatus() {
  VLOG(3) << "Received getStatus";
  return openr::thrift::ServiceStatus::ALIVE;
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
NetlinkServiceHandler::future_getRouteTableByClient(int16_t clientId) {
  LOG(INFO) << "Get routes from FIB for clientId " << clientId;

  return netlinkSocket_->getUnicastRoutes()
      .then([](UnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(res));
      })
      .onError([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get routing table by client: " << ex.what()
                   << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(UnicastRoutes({})));
      });
}

folly::Future<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
NetlinkServiceHandler::future_getKernelRouteTable() {
  LOG(INFO) << "Get routes from kernel";

  return netlinkSocket_->getKernelUnicastRoutes()
      .then([](UnicastRoutes res) mutable {
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(res));
      })
      .onError([](std::exception const& ex) {
        LOG(ERROR) << "Failed to get routing table by client: " << ex.what()
                   << ", returning empty table instead";
        return std::make_unique<std::vector<openr::thrift::UnicastRoute>>(
            makeRoutes(UnicastRoutes({})));
      });
}

void
NetlinkServiceHandler::getCounters(std::map<std::string, int64_t>& counters) {
  auto routes = netlinkSocket_->getUnicastRoutes().get();
  counters["fibagent.num_of_routes"] = routes.size();
}

} // namespace openr
