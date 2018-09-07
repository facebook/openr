/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkSubscriber.h"
#include "NetlinkException.h"
#include "NetlinkTypes.h"

#include <algorithm>
#include <array>
#include <set>
#include <thread>
#include <vector>

#include <folly/Format.h>
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/futures/Future.h>

namespace {
const folly::StringPiece kLinkObjectStr("route/link");
const folly::StringPiece kNeighborObjectStr("route/neigh");
const folly::StringPiece kAddrObjectStr("route/addr");
const folly::StringPiece kRouteObjectStr("route/route");
// We currently only handle v6 neighbor entries.
const uint8_t kFilterRouteFamily = AF_INET6;
// Socket buffer size for netlink sockets we create
// We use 2MB, default is 32KB
const size_t kNlSockRecvBuf{2 * 1024 * 1024};

// NUD_REACHABLE    a confirmed working cache entry
// NUD_STALE        an expired cache entry
// NUD_DELAY        an entry waiting for a timer
// NUD_PROBE        a cache entry that is currently reprobed
// NUD_PERMANENT    a static entry
// NUD_NOARP        a device with no destination cache
// NUD_INCOMPLETE   a currently resolving cache entry
// NUD_FAILED       an invalid cache entry
const std::set<int> kNeighborReachableStates{
    NUD_REACHABLE, NUD_STALE, NUD_DELAY, NUD_PERMANENT, NUD_PROBE, NUD_NOARP};

bool
isNeighborReachable(int state) {
  return kNeighborReachableStates.count(state);
}

std::string
ifIndexToName(struct nl_cache* linkCache, int ifIndex) {
  std::array<char, IFNAMSIZ> ifNameBuf;
  const char* ifNameStr =
      rtnl_link_i2name(linkCache, ifIndex, ifNameBuf.data(), ifNameBuf.size());
  if (!ifNameStr) {
    throw openr::NetlinkException(
        folly::sformat("Unknown interface index {}", ifIndex));
  }
  return ifNameStr;
}

// Helper routine to convert libnl Object into our Link entry
folly::Optional<openr::LinkEntry>
buildLink(struct nl_object* obj, bool deleted) {
  CHECK(obj) << "Invalid object pointer";
  struct rtnl_link* link = reinterpret_cast<struct rtnl_link*>(obj);

  const char* objectStr = nl_object_get_type(obj);
  if (objectStr && (objectStr != kLinkObjectStr)) {
    LOG(ERROR) << "Invalid nl_object type: " << nl_object_get_type(obj);
    return folly::none;
  }

  unsigned int flags = rtnl_link_get_flags(link);
  bool isUp = deleted ? false : !!(flags & IFF_RUNNING);
  std::string ifName("unknown");
  const char* ifNameStr = rtnl_link_get_name(link);
  if (ifNameStr) {
    ifName.assign(ifNameStr);
  }
  VLOG(4) << folly::sformat(
      "Link {} ({}) -> isUp ? {} : IFI_FLAGS: 0x{:0x}",
      ifName.c_str(),
      (deleted ? "deleted" : "added/updated"),
      isUp,
      flags);

  openr::LinkEntry linkEntry{
      std::move(ifName), isUp, rtnl_link_get_ifindex(link)};

  return linkEntry;
}

// Helper routine to convert libnl Object into our Neighbor entry
folly::Optional<openr::NeighborEntry>
buildNeighbor(struct nl_object* obj, struct nl_cache* linkCache, bool deleted) {
  CHECK(obj) << "Invalid object pointer";
  CHECK(linkCache) << "Invalid link cache";
  struct rtnl_neigh* neighbor = reinterpret_cast<struct rtnl_neigh*>(obj);

  const char* objectStr = nl_object_get_type(obj);
  if (objectStr && (objectStr != kNeighborObjectStr)) {
    LOG(ERROR) << "Invalid nl_object type: " << objectStr;
    return folly::none;
  }
  if (rtnl_neigh_get_family(neighbor) != kFilterRouteFamily) {
    VLOG(3) << "Skipping entries of non AF_INET6 family";
    return folly::none;
  }

  // The destination IP
  struct nl_addr* dst = rtnl_neigh_get_dst(neighbor);
  if (!dst) {
    LOG(ERROR) << "Invalid destination for neighbor";
    throw openr::NetlinkException(
        "Failed to get destination IP from neighbor entry");
  }
  const auto ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(dst)),
      nl_addr_get_len(dst)));

  const std::string ifName = ifIndexToName(
      linkCache, rtnl_neigh_get_ifindex(neighbor));
  bool isReachable =
      deleted ? false : isNeighborReachable(rtnl_neigh_get_state(neighbor));

  // link address exists only for reachable states, so it may not
  // always exist
  folly::MacAddress macAddress;
  if (isReachable) {
    struct nl_addr* linkAddress = rtnl_neigh_get_lladdr(neighbor);
    if (!linkAddress) {
      LOG(ERROR) << "Invalid link address for neigbbor";
      throw openr::NetlinkException(
          "Failed to get link address from neighbor entry");
    }
    // Skip entries with invalid mac-addresses
    if (nl_addr_get_len(linkAddress) != 6) {
      return folly::none;
    }
    macAddress = folly::MacAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(linkAddress)),
        nl_addr_get_len(linkAddress)));
  }

  openr::NeighborEntry neighborEntry{
      ifName, ipAddress, macAddress, isReachable};

  std::array<char, 128> stateBuf = {""};
  VLOG(4)
      << "Built neighbor entry: " << (deleted ? "(deleted)" : "(added/updated")
      << " family " << rtnl_neigh_get_family(neighbor) << " " << ifName << " : "
      << ipAddress.str() << " -> " << macAddress.toString() << " isReachable ? "
      << isReachable << " state "
      << rtnl_neigh_state2str(
             rtnl_neigh_get_state(neighbor), stateBuf.data(), stateBuf.size());

  return neighborEntry;
}

// Helper routine to convert libnl Object into our Link entry
folly::Optional<openr::AddrEntry>
buildAddr(struct nl_object* obj, struct nl_cache* linkCache, bool deleted) {
  CHECK(obj) << "Invalid object pointer";
  CHECK(linkCache) << "Invalid link cache";
  struct rtnl_addr* addr = reinterpret_cast<struct rtnl_addr*>(obj);

  const char* objectStr = nl_object_get_type(obj);
  if (objectStr && objectStr != kAddrObjectStr) {
    LOG(ERROR) << "Invalid nl_object type: " << objectStr;
    return folly::none;
  }

  const std::string ifName = ifIndexToName(
      linkCache, rtnl_addr_get_ifindex(addr));
  struct nl_addr* ipaddr = rtnl_addr_get_local(addr);
  if (!ipaddr) {
    LOG(ERROR) << "Invalid ip address for link " << ifName;
    throw openr::NetlinkException("Failed to get ip address for link" + ifName);
  }
  folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
      nl_addr_get_len(ipaddr)));

  uint8_t netmask = nl_addr_get_prefixlen(ipaddr);

  VLOG(4) << folly::sformat(
      "Addr {}/{} on link {} ({})",
      ipAddress.str(),
      std::to_string(netmask),
      ifName,
      (deleted ? "deleted" : "added/updated"));

  return openr::AddrEntry{
      std::move(ifName), {std::move(ipAddress), netmask}, !deleted};
}

folly::Optional<openr::RouteEntry>
buildRoute(struct nl_object* obj, struct nl_cache* linkCache, bool deleted) {
  CHECK(obj) << "Invalid object pointer";
  CHECK(linkCache) << "Invalid link cache";
  struct rtnl_route* routeObj = reinterpret_cast<struct rtnl_route*>(obj);
  const char* objectStr = nl_object_get_type(obj);
  if (objectStr && objectStr != kRouteObjectStr) {
    LOG(ERROR) << "Invalid nl_object type: " << objectStr
               << " expect: " << kRouteObjectStr.data();
    return folly::none;
  }

  try {
    openr::fbnl::RouteBuilder builder;
    openr::fbnl::Route route = builder.buildFromObject(routeObj);
    openr::RouteEntry routeEntry(
      route.getDestination(), route.getScope(), route.getRouteTable(),
      route.getProtocolId(), route.getType(), deleted);
    for (const auto& nextHop : route.getNextHops()) {
      if (!nextHop.getIfIndex().hasValue()
       || !nextHop.getGateway().hasValue()) {
         continue;
      }
      std::string ifName =
        ifIndexToName(linkCache, nextHop.getIfIndex().value());
      routeEntry.nexthops.emplace(ifName, nextHop.getGateway().value());
    }
    return routeEntry;
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Parsing route object failed";
    return folly::none;
  }
}

} // anonymous namespace

namespace openr {

NetlinkSubscriber::NetlinkSubscriber(
    fbzmq::ZmqEventLoop* zmqLoop, NetlinkSubscriber::Handler* handler)
    : zmqLoop_(zmqLoop), handler_(handler) {
  CHECK(zmqLoop != nullptr) << "Missing event loop.";
  CHECK(handler != nullptr) << "Missing subscription handler.";

  // Create netlink socket for only notification subscription
  subNlSock_ = nl_socket_alloc();
  CHECK(subNlSock_ != nullptr) << "Failed to create netlink socket.";

  // Create netlink socket for periodic refresh of our caches (link/addr/neigh)
  reqNlSock_ = nl_socket_alloc();
  CHECK(reqNlSock_ != nullptr) << "Failed to create netlink socket.";

  int err = nl_connect(reqNlSock_, NETLINK_ROUTE);
  CHECK_EQ(err, 0) << "Failed to connect nl socket. Error " << nl_geterror(err);

  // Create cache manager using notification socket
  err = nl_cache_mngr_alloc(
      subNlSock_, NETLINK_ROUTE, NL_AUTO_PROVIDE, &cacheManager_);
  CHECK_EQ(err, 0)
    << "Failed to create cache manager. Error: " << nl_geterror(err);

  // Set high buffers on netlink socket (especially on sub socket) so that
  // bulk events can also be received
  err = nl_socket_set_buffer_size(reqNlSock_, kNlSockRecvBuf, 0);
  CHECK_EQ(err, 0) << "Failed to set socket buffer on reqNlSock_";
  err = nl_socket_set_buffer_size(subNlSock_, kNlSockRecvBuf, 0);
  CHECK_EQ(err, 0) << "Failed to set socket buffer on subNlSock_";


  // Request a neighbor cache to be created and registered with cache manager
  // neighbor event handler is provided which has this object as opaque data so
  // we can get object state back in this static callback
  err = nl_cache_mngr_add(
      cacheManager_,
      kNeighborObjectStr.data(),
      neighborEventFunc,
      this,
      &neighborCache_);
  if (err != 0 || !neighborCache_) {
    CHECK(false)
      << "Failed to add neighbor cache to manager. Error: " << nl_geterror(err);
  }

  // For backward compitability,subscribe addr, link and neigh events by default
  subscribeEvent(fbnl::ADDR_EVENT);
  subscribeEvent(fbnl::LINK_EVENT);
  subscribeEvent(fbnl::NEIGH_EVENT);

  // Add link cache to manager. Same caveats as for neighborEventFunc
  err = nl_cache_mngr_add(
      cacheManager_, kLinkObjectStr.data(), linkEventFunc, this, &linkCache_);
  if (err != 0 || !linkCache_) {
    CHECK(false)
      << "Failed to add link cache to manager. Error: " << nl_geterror(err);
  }

  // Add address cache to manager. Same caveats as for neighborEventFunc
  err = nl_cache_mngr_add(
      cacheManager_, kAddrObjectStr.data(), addrEventFunc, this, &addrCache_);
  if (err != 0 || !addrCache_) {
    CHECK(false)
      << "Failed to add addr cache to manager. Error: " << nl_geterror(err);
  }

  err = nl_cache_mngr_add(
    cacheManager_, kRouteObjectStr.data(), routeEventFunc, this, &routeCache_);
  if (err != 0 || !routeCache_) {
    CHECK(false) << "Failed to add route cache to manager. Error: "
                 << nl_geterror(err);
  }

  // Get socket FD to monitor for updates
  int socketFd = nl_cache_mngr_get_fd(cacheManager_);
  CHECK_NE(socketFd, -1) << "Failed to get socket fd";

  // Anytime this socket has data, have libnl process it
  // Our registered handlers will be invoked..
  zmqLoop_->addSocketFd(socketFd, POLLIN, [this](int) noexcept {
    int lambdaErr = nl_cache_mngr_data_ready(cacheManager_);
    if (lambdaErr < 0) {
      LOG(ERROR) << "Error processing data on netlink socket. Error: "
                 << nl_geterror(lambdaErr);
    } else {
      VLOG(2) << "Processed " << lambdaErr << " netlink messages.";
    }
  });
}

NetlinkSubscriber::~NetlinkSubscriber() {
  VLOG(2) << "Destroying cache we created";

  zmqLoop_->removeSocketFd(nl_cache_mngr_get_fd(cacheManager_));

  // Manager will release our caches internally
  nl_cache_mngr_free(cacheManager_);
  nl_socket_free(subNlSock_);
  nl_socket_free(reqNlSock_);

  neighborCache_ = nullptr;
  linkCache_ = nullptr;
  addrCache_ = nullptr;
  cacheManager_ = nullptr;
  subNlSock_ = nullptr;
  reqNlSock_ = nullptr;
}

Links
NetlinkSubscriber::getAllLinks() {
  VLOG(3) << "Getting links";

  folly::Promise<Links> promise;
  auto future = promise.getFuture();

  zmqLoop_->runImmediatelyOrInEventLoop(
    [this, promise = std::move(promise)] () mutable {
      nl_cache_refill(reqNlSock_, linkCache_);
      nl_cache_refill(reqNlSock_, addrCache_);
      updateLinkAddrCache();

      promise.setValue(links_);
    });

  return std::move(future).get();
}

Neighbors
NetlinkSubscriber::getAllReachableNeighbors() {
  VLOG(3) << "Getting neighbors";

  folly::Promise<Neighbors> promise;
  auto future = promise.getFuture();

  zmqLoop_->runImmediatelyOrInEventLoop(
    [this, promise = std::move(promise)] () mutable {
      // Neighbor uses linkcache to map ifIndex to name
      // we really dont need to update addrCache_ but
      // no harm doing it since updateLinkAddrCache will update both
      nl_cache_refill(reqNlSock_, linkCache_);
      nl_cache_refill(reqNlSock_, addrCache_);
      nl_cache_refill(reqNlSock_, neighborCache_);
      updateLinkAddrCache();
      updateNeighborCache();

      promise.setValue(neighbors_);
    });

  return std::move(future).get();
}

void NetlinkSubscriber::subscribeEvent(fbnl::NetlinkEventType event) {
  if (event >= fbnl::MAX_EVENT_TYPE) {
    return;
  }
  eventFlags_.set(event);
}

void NetlinkSubscriber::unsubscribeEvent(
    fbnl::NetlinkEventType event) {
  if (event >= fbnl::MAX_EVENT_TYPE) {
    return;
  }
  eventFlags_.reset(event);
}

void NetlinkSubscriber::subscribeAllEvents() {
  for (size_t i = 0; i < fbnl::MAX_EVENT_TYPE; ++i) {
    eventFlags_.set(i);
  }
}

void NetlinkSubscriber::unsubscribeAllEvents() {
  for (size_t i = 0; i < fbnl::MAX_EVENT_TYPE; ++i) {
    eventFlags_.reset(i);
  }
}

// Invoked from libnl data processing callback whenever there
// is data on the socket
void
NetlinkSubscriber::handleLinkEvent(
    nl_object* obj, bool deleted, bool runHandler) noexcept {
  try {
    auto linkEntry = buildLink(obj, deleted);
    if (!linkEntry) {
      return;
    }
    auto& linkObj = links_[linkEntry->ifName];
    linkObj.isUp = linkEntry->isUp;
    linkObj.ifIndex = linkEntry->ifIndex;
    if (!linkEntry->isUp) {
      removeNeighborCacheEntries(linkEntry->ifName);
    }

    // Invoke handler
    if (runHandler && eventFlags_[fbnl::LINK_EVENT]) {
      VLOG(2) << "Link Event - ifName: " << linkEntry->ifName
              << ", ifIndex: " << linkEntry->ifIndex
              << ", state: " << (linkEntry->isUp ? "up" : "down");
      handler_->linkEventFunc(*linkEntry);
    }
  } catch (std::exception const& e) {
    LOG(ERROR) << "Error building link entry / invoking registered handler: "
               << folly::exceptionStr(e);
  }
}

void
NetlinkSubscriber::handleNeighborEvent(
    nl_object* obj, bool deleted, bool runHandler) noexcept {
  try {
    auto neighborEntry = buildNeighbor(obj, linkCache_, deleted);
    if (!neighborEntry) {
      return;
    }
    const auto neighborKey =
        std::make_pair(neighborEntry->ifName, neighborEntry->destination);
    if (neighborEntry->isReachable) {
      neighbors_[neighborKey] = neighborEntry->linkAddress;
    } else {
      neighbors_.erase(neighborKey);
    }

    // Invoke handler
    if (runHandler && eventFlags_[fbnl::NEIGH_EVENT]) {
      VLOG(2) << "Neigbbor Event - ifName: " << neighborEntry->ifName
              << ", macAddr: " << neighborEntry->destination
              << ", address: " << neighborEntry->linkAddress
              << ", state: "
              << (neighborEntry->isReachable ? " Reachable" : " Unreachable");
      handler_->neighborEventFunc(*neighborEntry);
    }
  } catch (std::exception const& e) {
    LOG(ERROR) << "Error building neighbor entry/invoking registered handler: "
               << folly::exceptionStr(e);
  }
}

void NetlinkSubscriber::handleRouteEvent(
    nl_object *obj, bool deleted, bool runHandler) noexcept {
  auto routeEntry = buildRoute(obj, linkCache_, deleted);
  if (!routeEntry) {
    return;
  }

  if (runHandler && eventFlags_[fbnl::ROUTE_EVENT]) {
    VLOG(2) << "Route Event - Prefix: "
            << folly::IPAddress::networkToString(routeEntry->prefix)
            << (routeEntry->isDeleted ? " Deleted" : " Added");
    handler_->routeEventFunc(*routeEntry);
  }
}

void
NetlinkSubscriber::handleAddrEvent(
    nl_object* obj, bool deleted, bool runHandler) noexcept {
  try {
    auto addrEntry = buildAddr(obj, linkCache_, deleted);
    if (!addrEntry) {
      return;
    }
    if (addrEntry->isValid) {
      links_[addrEntry->ifName].networks.insert(addrEntry->network);
    } else {
      auto it = links_.find(addrEntry->ifName);
      if (it != links_.end()) {
        it->second.networks.erase(addrEntry->network);
      }
    }

    // Invoke handler
    if (runHandler && eventFlags_[fbnl::ADDR_EVENT]) {
      VLOG(2) << "Address Event - ifName: " << addrEntry->ifName
              << ", address: " << addrEntry->network.first.str()
              << ", state: " << (addrEntry->isValid ? " Added" : " Removed");
      handler_->addrEventFunc(*addrEntry);
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "Error building addr entry/invoking registered handler: "
               << folly::exceptionStr(e);
  }
}

void
NetlinkSubscriber::linkEventFunc(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist";
  const bool deleted = (action == NL_ACT_DEL);
  reinterpret_cast<NetlinkSubscriber*>(data)->handleLinkEvent(
      obj, deleted, true);
}

void
NetlinkSubscriber::neighborEventFunc(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist";
  const bool deleted = (action == NL_ACT_DEL);
  reinterpret_cast<NetlinkSubscriber*>(data)->handleNeighborEvent(
      obj, deleted, true);
}

void
NetlinkSubscriber::addrEventFunc(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept {
  CHECK(data) << "Opaque context does not exist";
  const bool deleted = (action == NL_ACT_DEL);
  reinterpret_cast<NetlinkSubscriber*>(data)->handleAddrEvent(
      obj, deleted, true);
}

void
NetlinkSubscriber::routeEventFunc(
    struct nl_cache *, struct nl_object *obj, int action, void *data) noexcept {
  CHECK(data) << "Opaque context does not exist";
  bool deleted = (action == NL_ACT_DEL);
  reinterpret_cast<NetlinkSubscriber*>(data)->handleRouteEvent(
      obj, deleted, true);
}

void
NetlinkSubscriber::updateLinkAddrCache() {
  auto linkFunc = [](struct nl_object * obj, void* arg) noexcept->void {
    CHECK(arg) << "Opaque context does not exist";
    reinterpret_cast<NetlinkSubscriber*>(arg)->handleLinkEvent(
        obj, false, false);
  };
  nl_cache_foreach_filter(linkCache_, nullptr, linkFunc, this);

  auto addrFunc = [](struct nl_object * obj, void* arg) noexcept {
    CHECK(arg) << "Opaque context does not exist";
    reinterpret_cast<NetlinkSubscriber*>(arg)->handleAddrEvent(
        obj, false, false);
  };
  nl_cache_foreach_filter(addrCache_, nullptr, addrFunc, this);
}

void
NetlinkSubscriber::updateNeighborCache() {
  auto neighborFunc = [](struct nl_object * obj, void* arg) noexcept {
    CHECK(arg) << "Opaque context does not exist";
    reinterpret_cast<NetlinkSubscriber*>(arg)->handleNeighborEvent(
        obj, false, false);
  };
  nl_cache_foreach_filter(neighborCache_, nullptr, neighborFunc, this);
}

void
NetlinkSubscriber::removeNeighborCacheEntries(const std::string& ifName) {
  for (auto it = neighbors_.begin(); it != neighbors_.end();) {
    if (std::get<0>(it->first) == ifName) {
      it = neighbors_.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace openr
