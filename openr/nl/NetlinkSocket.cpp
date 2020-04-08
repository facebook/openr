/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkSocket.h>
#include <openr/if/gen-cpp2/Platform_constants.h>

namespace openr::fbnl {

NetlinkSocket::NetlinkSocket(
    fbzmq::ZmqEventLoop* evl,
    EventsHandler* handler,
    std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlSock)
    : evl_(evl), handler_(handler), nlSock_(std::move(nlSock)) {
  CHECK(evl_ != nullptr) << "Missing event loop.";

  CHECK(nlSock_ != nullptr) << "Missing NetlinkProtocolSocket";

  // Instantiate local link and neighbor caches
  getAllReachableNeighbors().get();

  // Pass link and address callbacks to NetlinkProtocolSocket
  nlSock_->setLinkEventCB([this](
      openr::fbnl::Link link, bool runHandler) noexcept {
    evl_->runImmediatelyOrInEventLoop([this,
                                       link = std::move(link),

                                       runHandler = runHandler]() mutable {
      doHandleLinkEvent(link, runHandler);
    });
  });

  nlSock_->setAddrEventCB([this](
      openr::fbnl::IfAddress ifAddr, bool runHandler) noexcept {
    evl_->runImmediatelyOrInEventLoop([this,
                                       ifAddr = std::move(ifAddr),

                                       runHandler = runHandler]() mutable {
      doHandleAddrEvent(ifAddr, runHandler);
    });
  });

  nlSock_->setNeighborEventCB([this](
      openr::fbnl::Neighbor neigh, bool runHandler) noexcept {
    evl_->runImmediatelyOrInEventLoop([this,
                                       neigh = std::move(neigh),

                                       runHandler = runHandler]() mutable {
      doHandleNeighborEvent(neigh, runHandler);
    });
  });

  // need to reload routes from kernel to avoid re-adding existing route
  // type of exception in NetlinkSocket
  updateRouteCache();
}

NetlinkSocket::~NetlinkSocket() {
  nlSock_.reset();
}

void
NetlinkSocket::doHandleRouteEvent(
    Route route, bool runHandler, bool updateUnicastRoute) noexcept {
  auto routeCopy = Route(route);
  try {
    doUpdateRouteCache(std::move(route), updateUnicastRoute);
  } catch (const folly::InvalidAddressFamilyException& ex) {
    // Empty address in route. Ignore exception.
    return;
  } catch (const std::exception& ex) {
    LOG(ERROR) << "UpdateCacheFailed";
  }

  if (handler_ && runHandler && eventFlags_[ROUTE_EVENT]) {
    std::string ifName = routeCopy.getRouteIfName().has_value()
        ? routeCopy.getRouteIfName().value()
        : "";
    EventVariant event = std::move(routeCopy);
    handler_->handleEvent(ifName, event);
  }
}

void
NetlinkSocket::doHandleLinkEvent(Link link, bool runHandler) noexcept {
  const auto linkName = link.getLinkName();
  auto& linkAttr = links_[linkName];
  linkAttr.isUp = link.isUp();
  linkAttr.ifIndex = link.getIfIndex();
  if (link.isLoopback()) {
    loopbackIfIndex_ = linkAttr.ifIndex;
  }
  if (!linkAttr.isUp) {
    removeNeighborCacheEntries(linkName);
  }
  if (handler_ && runHandler && eventFlags_[LINK_EVENT]) {
    EventVariant event = std::move(link);
    handler_->handleEvent(linkName, event);
  }
}

void
NetlinkSocket::removeNeighborCacheEntries(const std::string& ifName) {
  for (auto it = neighbors_.begin(); it != neighbors_.end();) {
    if (std::get<0>(it->first) == ifName) {
      it = neighbors_.erase(it);
    } else {
      ++it;
    }
  }
}

void
NetlinkSocket::doHandleAddrEvent(IfAddress ifAddr, bool runHandler) noexcept {
  std::string ifName = getIfName(ifAddr.getIfIndex()).get();
  if (ifAddr.isValid()) {
    if (links_[ifName].networks.count(ifAddr.getPrefix().value()) == 0) {
      links_[ifName].networks.insert(ifAddr.getPrefix().value());
    }
  } else if (!ifAddr.isValid()) {
    auto it = links_.find(ifName);
    if (it != links_.end()) {
      it->second.networks.erase(ifAddr.getPrefix().value());
    }
  }

  if (handler_ && runHandler && eventFlags_[ADDR_EVENT]) {
    EventVariant event = std::move(ifAddr);
    handler_->handleEvent(ifName, event);
  }
}

void
NetlinkSocket::doHandleNeighborEvent(
    Neighbor neighbor, bool runHandler) noexcept {
  std::string ifName = getIfName(neighbor.getIfIndex()).get();
  auto key = std::make_pair(ifName, neighbor.getDestination());
  neighbors_.erase(key);

  NeighborUpdate neighborUpdate;
  if (neighbor.isReachable()) {
    neighbors_.emplace(std::make_pair(key, neighbor));
    neighborUpdate.addNeighbor(neighbor.getDestination().str());
  } else {
    neighborUpdate.delNeighbor(neighbor.getDestination().str());
  }

  if (neighborListener_) {
    std::lock_guard<std::mutex> g(neighborListenerMutex_);
    try {
      neighborListener_(neighborUpdate);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "neighbor call failed: " << ex.what();
    }
  }

  if (handler_ && runHandler && eventFlags_[NEIGH_EVENT]) {
    NeighborBuilder nhBuilder;
    EventVariant event = std::move(neighbor);
    handler_->handleEvent(ifName, event);
  }
}

void
NetlinkSocket::doUpdateRouteCache(Route route, bool updateUnicastRoute) {
  // Skip cached route entries and any routes not in the main table
  int flags = route.getFlags().has_value() ? route.getFlags().value() : 0;
  if (route.getRouteTable() != RT_TABLE_MAIN || flags & RTM_F_CLONED) {
    return;
  }

  const folly::CIDRNetwork& prefix = route.getDestination();

  if (prefix.first.isMulticast() or route.getScope() == RT_SCOPE_LINK) {
    return;
  }

  if (updateUnicastRoute) {
    auto& unicastRoutes = unicastRoutesCache_[route.getProtocolId()];
    if (route.isValid()) {
      unicastRoutes.erase(prefix);
      unicastRoutes.emplace(prefix, std::move(route));
    }
    // NOTE: We are just updating cache. This called during initialization
  }
}

folly::Future<folly::Unit>
NetlinkSocket::addRoute(Route route) {
  auto prefix = route.getDestination();
  VLOG(3) << "NetlinkSocket add route "
          << folly::IPAddress::networkToString(prefix);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     dest = std::move(prefix),
                                     r = std::move(route)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
      case RTN_BLACKHOLE:
        doAddUpdateUnicastRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error adding routes to "
                 << folly::IPAddress::networkToString(dest)
                 << ". Exception: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::addMplsRoute(Route mplsRoute) {
  auto prefix = mplsRoute.getDestination();
  VLOG(3) << "NetlinkSocket add MPLS route "
          << folly::IPAddress::networkToString(prefix);

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     dest = std::move(prefix),
                                     r = std::move(mplsRoute)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
        doAddUpdateMplsRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported MPLS route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error adding MPLS routes to "
                 << folly::IPAddress::networkToString(dest)
                 << ". Exception: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::delMplsRoute(Route mplsRoute) {
  VLOG(3) << "NetlinkSocket deleting MPLS route";
  auto prefix = mplsRoute.getDestination();
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     r = std::move(mplsRoute),
                                     dest = std::move(prefix)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
        doDeleteMplsRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported MPLS route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error deleting MPLS routes to "
                 << folly::IPAddress::networkToString(dest)
                 << " Error: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::syncMplsRoutes(uint8_t protocolId, NlMplsRoutes newMplsRouteDb) {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     syncDb = std::move(newMplsRouteDb),
                                     protocolId]() mutable {
    try {
      LOG(INFO) << "Syncing " << syncDb.size() << " mpls routes";
      auto& mplsRoutes = mplsRoutesCache_[protocolId];
      std::unordered_set<int32_t> toDelete;
      // collect label routes to delete
      for (auto const& kv : mplsRoutes) {
        if (syncDb.find(kv.first) == syncDb.end()) {
          toDelete.insert(kv.first);
        }
      }
      // delete
      LOG(INFO) << "Sync: Deleting " << toDelete.size() << " mpls routes";
      for (auto label : toDelete) {
        auto mplsRouteEntry = mplsRoutes.at(label);
        doDeleteMplsRoute(mplsRouteEntry);
      }
      // Go over MPLS routes in new routeDb, update/add
      for (auto& kv : syncDb) {
        doAddUpdateMplsRoute(kv.second);
      }
      p.setValue();
      LOG(INFO) << "Sync done.";
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error syncing MPLS routeDb with Fib: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<NlMplsRoutes>
NetlinkSocket::getCachedMplsRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket get cached MPLS routes by protocol "
          << (int)protocolId;
  folly::Promise<NlMplsRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), protocolId]() mutable {
        auto iter = mplsRoutesCache_.find(protocolId);
        if (iter != mplsRoutesCache_.end()) {
          p.setValue(iter->second);
        } else {
          p.setValue(NlMplsRoutes{});
        }
      });
  return future;
}

folly::Future<int64_t>
NetlinkSocket::getMplsRouteCount() const {
  VLOG(3) << "NetlinkSocket get MPLS routes count";

  folly::Promise<int64_t> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    int64_t count = 0;
    for (const auto& routes : mplsRoutesCache_) {
      count += routes.second.size();
    }
    p.setValue(count);
  });
  return future;
}

void
NetlinkSocket::doAddUpdateUnicastRoute(Route route) {
  checkUnicastRoute(route);

  const auto& dest = route.getDestination();

  // Create new set of nexthops to be programmed. Existing + New ones
  auto& unicastRoutes = unicastRoutesCache_[route.getProtocolId()];
  auto iter = unicastRoutes.find(dest);
  // if user did not speicify priority
  if (!route.getPriority()) {
    const auto routePair =
        openr::thrift::Platform_constants::protocolIdtoPriority().find(
            route.getProtocolId());
    if (routePair ==
        openr::thrift::Platform_constants::protocolIdtoPriority().end()) {
      route.setPriority(
          openr::thrift::Platform_constants::kUnknowProtAdminDistance());
    } else {
      route.setPriority(routePair->second);
    }
  }
  // Same route
  if (iter != unicastRoutes.end() && iter->second == route) {
    return;
  }

  if (dest.first.isV6()) {
    // We need to explicitly add new V6 routes & remove old routes
    // With IPv6, if new route being requested has different properties
    // (like gateway or metric or..) the existing one will not be replaced,
    // instead a new route will be created, which may cause underlying kernel
    // crash when releasing netdevices
    if (iter != unicastRoutes.end()) {
      int err{0};

      err = static_cast<int>(nlSock_->deleteRoute(iter->second));

      if (0 != err) {
        throw fbnl::NlException(folly::sformat(
            "Failed to delete route\n{}\nError: {}", iter->second.str(), err));
      }
    }
  }

  // Remove route from cache
  unicastRoutes.erase(dest);

  // Add new route
  int err{0};

  err = static_cast<int>(nlSock_->addRoute(route));

  if (0 != err) {
    throw fbnl::NlException(
        folly::sformat("Could not add route\n{}\nError: {}", route.str(), err));
  }

  // Add route entry in cache on successful addition
  unicastRoutes.emplace(std::make_pair(dest, std::move(route)));
}

folly::Future<folly::Unit>
NetlinkSocket::delRoute(Route route) {
  VLOG(3) << "NetlinkSocket deleting unicast route";
  auto prefix = route.getDestination();
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     r = std::move(route),
                                     dest = std::move(prefix)]() mutable {
    try {
      uint8_t type = r.getType();
      switch (type) {
      case RTN_UNICAST:
      case RTN_BLACKHOLE:
        doDeleteUnicastRoute(std::move(r));
        break;
      default:
        throw fbnl::NlException(
            folly::sformat("Unsupported route type {}", (int)type));
      }
      p.setValue();
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error deleting routes to "
                 << folly::IPAddress::networkToString(dest)
                 << " Error: " << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

void
NetlinkSocket::checkUnicastRoute(const Route& route) {
  const auto& prefix = route.getDestination();
  if (prefix.first.isMulticast() || prefix.first.isLinkLocal()) {
    throw fbnl::NlException(folly::sformat(
        "Invalid unicast route type for: {}",
        folly::IPAddress::networkToString(prefix)));
  }
}

void
NetlinkSocket::doDeleteMplsRoute(Route mplsRoute) {
  auto label = mplsRoute.getMplsLabel();
  if (!label.has_value()) {
    return;
  }
  auto& mplsRoutes = mplsRoutesCache_[mplsRoute.getProtocolId()];
  if (mplsRoutes.count(label.value()) == 0) {
    LOG(ERROR) << "Trying to delete non-existing label: " << label.value();
    return;
  }

  int err{0};
  err = static_cast<int>(nlSock_->deleteLabelRoute(mplsRoute));
  if (err != 0) {
    throw fbnl::NlException(folly::sformat(
        "Failed to delete MPLS {} Error: {}", label.value(), err));
  }
  // Update local cache with removed prefix
  mplsRoutes.erase(label.value());
}

void
NetlinkSocket::doAddUpdateMplsRoute(Route mplsRoute) {
  auto label = mplsRoute.getMplsLabel();
  if (!label.has_value()) {
    LOG(ERROR) << "MPLS route add - no label provided";
    return;
  }
  // check cache has the same entry
  auto& mplsRoutes = mplsRoutesCache_[mplsRoute.getProtocolId()];
  auto mplsRouteEntry = mplsRoutes.find(label.value());
  // Same route
  if (mplsRouteEntry != mplsRoutes.end() &&
      mplsRouteEntry->second == mplsRoute) {
    return;
  }

  mplsRoutes.erase(label.value());
  int err{0};
  err = static_cast<int>(nlSock_->addLabelRoute(mplsRoute));
  if (0 != err) {
    throw fbnl::NlException(folly::sformat(
        "Could not add mpls route\n{}\nError: {}", mplsRoute.str(), err));
  }
  // Add MPLS route entry in cache on successful addition
  mplsRoutes.emplace(std::make_pair(
      static_cast<int32_t>(label.value()), std::move(mplsRoute)));
}

void
NetlinkSocket::doDeleteUnicastRoute(Route route) {
  checkUnicastRoute(route);

  const auto& prefix = route.getDestination();
  auto& unicastRoutes = unicastRoutesCache_[route.getProtocolId()];
  if (unicastRoutes.count(prefix) == 0) {
    LOG(ERROR) << "Trying to delete non-existing prefix "
               << folly::IPAddress::networkToString(prefix);
    return;
  }

  int err{0};

  err = static_cast<int>(nlSock_->deleteRoute(route));
  if (err != 0) {
    throw fbnl::NlException(folly::sformat(
        "Failed to delete route {} Error: {}",
        folly::IPAddress::networkToString(route.getDestination()),
        err));
  }

  // Update local cache with removed prefix
  unicastRoutes.erase(route.getDestination());
}

folly::Future<folly::Unit>
NetlinkSocket::syncUnicastRoutes(
    uint8_t protocolId, NlUnicastRoutes newRouteDb) {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     syncDb = std::move(newRouteDb),
                                     protocolId]() mutable {
    try {
      LOG(INFO) << "Syncing " << syncDb.size() << " routes for protocol "
                << static_cast<int>(protocolId);
      doSyncUnicastRoutes(protocolId, std::move(syncDb));
      p.setValue();
      LOG(INFO) << "Sync done.";
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error syncing unicast routeDb with Fib: "
                 << folly::exceptionStr(ex);
      p.setException(ex);
    }
  });
  return future;
}

void
NetlinkSocket::doSyncUnicastRoutes(uint8_t protocolId, NlUnicastRoutes syncDb) {
  auto& unicastRoutes = unicastRoutesCache_[protocolId];

  // Go over routes that are not in new routeDb, delete
  std::unordered_set<folly::CIDRNetwork> toDelete;
  for (auto const& kv : unicastRoutes) {
    if (syncDb.find(kv.first) == syncDb.end()) {
      toDelete.insert(kv.first);
    }
  }
  // Delete routes from kernel
  LOG(INFO) << "Sync: number of routes to delete: " << toDelete.size();
  for (auto it = toDelete.begin(); it != toDelete.end(); ++it) {
    auto const& prefix = *it;
    auto iter = unicastRoutes.find(prefix);
    if (iter == unicastRoutes.end()) {
      continue;
    }
    doDeleteUnicastRoute(iter->second);
  }

  // Go over routes in new routeDb, update/add
  LOG(INFO) << "Sync: number of routes to add: " << syncDb.size();
  for (auto& kv : syncDb) {
    doAddUpdateUnicastRoute(kv.second);
  }
}

folly::Future<NlUnicastRoutes>
NetlinkSocket::getCachedUnicastRoutes(uint8_t protocolId) const {
  VLOG(3) << "NetlinkSocket getCachedUnicastRoutes by protocol "
          << (int)protocolId;
  folly::Promise<NlUnicastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), protocolId]() mutable {
        auto iter = unicastRoutesCache_.find(protocolId);
        if (iter != unicastRoutesCache_.end()) {
          p.setValue(iter->second);
        } else {
          p.setValue(NlUnicastRoutes{});
        }
      });
  return future;
}

folly::Future<int64_t>
NetlinkSocket::getRouteCount() const {
  VLOG(3) << "NetlinkSocket get routes number";

  folly::Promise<int64_t> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    int64_t count = 0;
    for (const auto& routes : unicastRoutesCache_) {
      count += routes.second.size();
    }
    p.setValue(count);
  });
  return future;
}

folly::Future<int>
NetlinkSocket::getIfIndex(const std::string& ifName) {
  folly::Promise<int> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifName = folly::copy(ifName)]() mutable {
        int ifIndex{-1};

        if (links_.count(ifName)) {
          ifIndex = links_[ifName].ifIndex;
        }

        p.setValue(ifIndex);
      });
  return future;
}

folly::Future<std::optional<int>>
NetlinkSocket::getLoopbackIfIndex() {
  folly::Promise<std::optional<int>> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    p.setValue(loopbackIfIndex_);
  });
  return future;
}

folly::Future<std::string>
NetlinkSocket::getIfName(int ifIndex) const {
  folly::Promise<std::string> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifIndex]() mutable {
        std::string ifName{""};

        for (const auto& linkEntry : links_) {
          if (linkEntry.second.ifIndex == ifIndex) {
            ifName = linkEntry.first;
          }
        }

        p.setValue(ifName);
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::addIfAddress(IfAddress ifAddress) {
  LOG(INFO) << "NetlinkSocket add IfAddress... " << ifAddress.str();

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), addr = std::move(ifAddress)]() mutable {
        int err = static_cast<int>(nlSock_->addIfAddress(addr));
        if (err == 0) {
          p.setValue();
        } else {
          p.setException(fbnl::NlException("Failed to add If Address"));
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::delIfAddress(IfAddress ifAddress) {
  LOG(INFO) << "Netlink delete IfAddress... " << ifAddress.str();

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  if (!ifAddress.getPrefix().has_value()) {
    fbnl::NlException ex("Prefix must be set");
    promise.setException(std::move(ex));
    return future;
  }
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifAddr = std::move(ifAddress)]() mutable {
        int err = static_cast<int>(nlSock_->deleteIfAddress(ifAddr));
        if (err == 0) {
          p.setValue();
        } else {
          p.setException(fbnl::NlException("Failed to delete If Address"));
        }
      });
  return future;
}

folly::Future<folly::Unit>
NetlinkSocket::syncIfAddress(
    int ifIndex, std::vector<IfAddress> addresses, int family, int scope) {
  LOG(INFO) << "Netlink sync IfAddress...";

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this,
                                     p = std::move(promise),
                                     addrs = std::move(addresses),
                                     ifIndex,
                                     family,
                                     scope]() mutable {
    try {
      doSyncIfAddress(ifIndex, std::move(addrs), family, scope);
      p.setValue();
    } catch (const std::exception& ex) {
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<std::vector<IfAddress>>
NetlinkSocket::getIfAddrs(int ifIndex, int family, int scope) {
  VLOG(2) << "Netlink get IfaceAddrs...";

  folly::Promise<std::vector<IfAddress>> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop(
      [this, p = std::move(promise), ifIndex, family, scope]() mutable {
        std::vector<IfAddress> addrs;
        std::vector<fbnl::IfAddress> allAddrs = nlSock_->getAllIfAddresses();
        for (auto& addr : allAddrs) {
          // Check if addr fits the filtering parameters
          if (family != AF_UNSPEC && family != addr.getFamily()) {
            continue;
          }
          auto addrScope = addr.getScope().has_value() ? addr.getScope().value()
                                                       : 0; // RT_SCOPE_UNIVERSE
          if (scope != RT_SCOPE_NOWHERE && scope != addrScope) {
            continue;
          }
          if (ifIndex != addr.getIfIndex()) {
            continue;
          }
          if (!addr.getPrefix().has_value()) {
            continue;
          }

          fbnl::IfAddressBuilder ifBuilder;
          auto ifAddr = ifBuilder.setPrefix(std::move(addr.getPrefix().value()))
                            .setIfIndex(ifIndex)
                            .setScope(scope)
                            .build();
          addrs.emplace_back(std::move(ifAddr));
        }
        p.setValue(std::move(addrs));
      });
  return future;
}

void
NetlinkSocket::doSyncIfAddress(
    int ifIndex, std::vector<IfAddress> addrs, int family, int scope) {
  std::vector<folly::CIDRNetwork> newPrefixes;
  for (const auto& addr : addrs) {
    if (addr.getIfIndex() != ifIndex) {
      throw fbnl::NlException("Inconsistent ifIndex in addrs");
    }
    if (!addr.getPrefix().has_value()) {
      throw fbnl::NlException("Prefix must be set when sync addresses");
    }
    newPrefixes.emplace_back(addr.getPrefix().value());
  }

  std::vector<folly::CIDRNetwork> oldPrefixes;
  auto oldAddrs = getIfAddrs(ifIndex, family, scope).get();
  for (const auto& addr : oldAddrs) {
    oldPrefixes.emplace_back(addr.getPrefix().value());
  }

  PrefixCmp cmp;
  sort(newPrefixes.begin(), newPrefixes.end(), cmp);
  sort(oldPrefixes.begin(), oldPrefixes.end(), cmp);

  // get a list of prefixes need to be deleted
  std::vector<folly::CIDRNetwork> toDeletePrefixes;
  std::set_difference(
      oldPrefixes.begin(),
      oldPrefixes.end(),
      newPrefixes.begin(),
      newPrefixes.end(),
      std::inserter(toDeletePrefixes, toDeletePrefixes.begin()));

  // Do add first, because in Linux deleting the only IP will cause if down.
  // Add new address, existed addresses will be ignored
  for (auto& addr : addrs) {
    addIfAddress(addr);
  }

  // Delete deprecated addresses
  fbnl::IfAddressBuilder builder;
  for (const auto& toDel : toDeletePrefixes) {
    auto delAddr =
        builder.setIfIndex(ifIndex).setPrefix(toDel).setScope(scope).build();
    delIfAddress(delAddr);
  }
}

folly::Future<NlLinks>
NetlinkSocket::getAllLinks() {
  VLOG(3) << "NetlinkSocket get all links...";
  folly::Promise<NlLinks> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    try {
      auto links = nlSock_->getAllLinks();
      for (auto& link : links) {
        doHandleLinkEvent(link, false);
      }
      auto addresses = nlSock_->getAllIfAddresses();
      for (auto& address : addresses) {
        doHandleAddrEvent(address, false);
      }
      p.setValue(links_);
    } catch (const std::exception& ex) {
      p.setException(ex);
    }
  });
  return future;
}

folly::Future<NlNeighbors>
NetlinkSocket::getAllReachableNeighbors() {
  VLOG(3) << "NetlinkSocket get neighbors...";
  folly::Promise<NlNeighbors> promise;
  auto future = promise.getFuture();
  evl_->runImmediatelyOrInEventLoop([this, p = std::move(promise)]() mutable {
    try {
      getAllLinks().get();

      auto neighbors = nlSock_->getAllNeighbors();
      for (auto& neighbor : neighbors) {
        doHandleNeighborEvent(neighbor, false);
      }

      p.setValue(neighbors_);
    } catch (const std::exception& ex) {
      p.setException(ex);
    }
  });
  return future;
}

void
NetlinkSocket::updateRouteCache() {
  auto routes = nlSock_->getAllRoutes();
  for (auto& route : routes) {
    doHandleRouteEvent(route, false, true);
  }
}

std::vector<fbnl::Route>
NetlinkSocket::getAllRoutes() const {
  return nlSock_->getAllRoutes();
}

void
NetlinkSocket::subscribeEvent(NetlinkEventType event) {
  if (event >= MAX_EVENT_TYPE) {
    return;
  }
  eventFlags_.set(event);
}

void
NetlinkSocket::unsubscribeEvent(NetlinkEventType event) {
  if (event >= MAX_EVENT_TYPE) {
    return;
  }
  eventFlags_.reset(event);
}

void
NetlinkSocket::subscribeAllEvents() {
  for (size_t i = 0; i < MAX_EVENT_TYPE; ++i) {
    eventFlags_.set(i);
  }
}

void
NetlinkSocket::unsubscribeAllEvents() {
  for (size_t i = 0; i < MAX_EVENT_TYPE; ++i) {
    eventFlags_.reset(i);
  }
}

void
NetlinkSocket::setEventHandler(EventsHandler* handler) {
  handler_ = handler;
}

void
NetlinkSocket::registerNeighborListener(
    std::function<void(const NeighborUpdate& neighborUpdate)> callback) {
  std::lock_guard<std::mutex> g(neighborListenerMutex_);
  neighborListener_ = std::move(callback);
}

} // namespace openr::fbnl
