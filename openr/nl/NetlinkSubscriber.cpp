/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkSubscriber.h"
#include "NetlinkException.h"

#include <algorithm>
#include <array>
#include <set>
#include <thread>
#include <vector>

#include <folly/Format.h>
#include <folly/Optional.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>

namespace {
const folly::StringPiece kLinkObjectStr("route/link");
const folly::StringPiece kNeighborObjectStr("route/neigh");
const folly::StringPiece kAddrObjectStr("route/addr");
// We currently only handle v6 neighbor entries.
const uint8_t kFilterRouteFamily = AF_INET6;

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


int
ifNameToIndex(struct nl_cache* linkCache, std::string ifName) {
  auto ifIndex =
      rtnl_link_name2i(linkCache, ifName.c_str());
  if (!ifIndex) {
    throw openr::NetlinkException(
        folly::sformat("Unknown interface Name {}", ifName));
  }
  return ifIndex;
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

  std::string ifName =
      ifIndexToName(linkCache, rtnl_neigh_get_ifindex(neighbor));
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

  std::string ifName = ifIndexToName(linkCache, rtnl_addr_get_ifindex(addr));
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

} // anonymous namespace

namespace openr {

NetlinkSubscriber::NetlinkSubscriber(
    fbzmq::ZmqEventLoop* zmqLoop, NetlinkSubscriber::Handler* handler)
    : zmqLoop_(zmqLoop), handler_(handler) {
  if (!zmqLoop_ || !handler_) {
    throw NetlinkException("Invalid args for creating NetlinkSubscriber");
  }

  sock_ = nl_socket_alloc();
  if (!sock_) {
    throw NetlinkException(folly::sformat("Failed to create netlink socket"));
  }

  SCOPE_FAIL {
    nl_socket_free(sock_);
  };

  int err = nl_cache_mngr_alloc(
      sock_, NETLINK_ROUTE, NL_AUTO_PROVIDE, &cacheManager_);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to create cache Manager. Error: {}", nl_geterror(err)));
  }

  SCOPE_FAIL {
    nl_cache_mngr_free(cacheManager_);
  };

  // Request a neighbor cache to be created and registered with cache manager
  // neighbor event handler is provided which has this object as opaque data so
  // we can get object state back in this static callback
  // NOTE:
  // We store this object as opaque data into libnl so we can access linkCache_
  // to map ifIndex to link name.
  // We can create a context with just user handler and linkCache_ but
  // for now, lets just give the entire object to libnl as opaque data
  // We are careful not to call methods from the libnl callback for fear of
  // race conditions. Call to dataReady() will implicitly update the
  // linkCache_ and use it as read-only in the libnl callback
  err = nl_cache_mngr_add(
      cacheManager_,
      kNeighborObjectStr.data(),
      neighborEventFunc,
      this,
      &neighborCache_);

  if (err != 0 || !neighborCache_) {
    throw NetlinkException(folly::sformat(
        "Failed to add neighbor cache to manager. Error: {}",
        nl_geterror(err)));
  }

  // Add link cache to manager. Same caveats as for neighborEventFunc
  err = nl_cache_mngr_add(
      cacheManager_, kLinkObjectStr.data(), linkEventFunc, this, &linkCache_);

  if (err != 0 || !linkCache_) {
    throw NetlinkException(folly::sformat(
        "Failed to add link cache to manager. Error: {}", nl_geterror(err)));
  }

  // Add address cache to manager. Same caveats as for neighborEventFunc
  err = nl_cache_mngr_add(
      cacheManager_, kAddrObjectStr.data(), addrEventFunc, this, &addrCache_);

  if (err != 0 || !addrCache_) {
    throw NetlinkException(folly::sformat(
        "Failed to add addr cache to manager. Error: {}", nl_geterror(err)));
  }

  int socketFd = nl_cache_mngr_get_fd(cacheManager_);
  if (socketFd == -1) {
    throw NetlinkException("Failed to get socket fd");
  }

  // Anytime this socket has data, have libnl process it
  // Our registered handlers will be invoked..
  zmqLoop_->addSocketFd(socketFd, POLLIN, [this](int) noexcept {
    try {
      dataReady();
    } catch (std::exception const& e) {
      LOG(ERROR) << "Error processing data on socket: "
                 << folly::exceptionStr(e);
      return;
    }
  });
}

NetlinkSubscriber::~NetlinkSubscriber() {
  VLOG(2) << "Destroying cache we created";

  zmqLoop_->removeSocketFd(nl_cache_mngr_get_fd(cacheManager_));
  // Manager will release our caches internally
  nl_cache_mngr_free(cacheManager_);
  nl_socket_free(sock_);
}

Links
NetlinkSubscriber::getAllLinks() {
  VLOG(3) << "Getting links";

  // Update kernel caches which will invoke registered handlers
  // where we update our local cache
  // This will happen in the calling thread context
  updateFromKernelCaches();

  std::lock_guard<std::mutex> lock(netlinkMutex_);

  VLOG(2) << "Refilling link cache";
  nl_cache_refill(sock_, linkCache_);
  VLOG(2) << "Refilling address cache";
  nl_cache_refill(sock_, addrCache_);

  fillLinkCache();
  return links_;
}

Neighbors
NetlinkSubscriber::getAllReachableNeighbors() {
  VLOG(3) << "Getting neighbors";

  // Update kernel caches which will invoke registered handlers
  // In those handlers we also update our local cache
  // This will happen in the calling thread context
  updateFromKernelCaches();

  std::lock_guard<std::mutex> lock(netlinkMutex_);

  // Neighbor uses linkcache to map ifIndex to name
  // we really dont need to update addrCache_ but
  // no harm doing it since fillLinkCache will update both
  VLOG(2) << "Refilling link cache";
  nl_cache_refill(sock_, linkCache_);
  VLOG(2) << "Refilling address cache";
  nl_cache_refill(sock_, addrCache_);
  VLOG(2) << "Refilling neighbor cache";
  nl_cache_refill(sock_, neighborCache_);

  fillLinkCache();
  fillNeighborCache();
  return neighbors_;
}


int
NetlinkSubscriber::addIpv6Neighbor(std::string ifname,
                                   std::string neigh_dst_addr) {
    // Allocate an empty neighbour handle to be filled out with the attributes
    // of the new neighbour.
    struct rtnl_neigh *neigh = rtnl_neigh_alloc();

    struct nl_addr* dst_addr;
    int addop;

    VLOG(2) << "Ready to add neigh!!: "
            << "ifname: " << ifname
            << " dst_addr: " << neigh_dst_addr;

    if (nl_addr_parse(neigh_dst_addr.c_str(), AF_INET6, &dst_addr) < 0) {
        return -1;
    }


    auto if_index = ifNameToIndex(linkCache_, ifname);

    VLOG(2) << "Filling out the attributes!!";

    // Fill out the attributes of the new neighbour
    rtnl_neigh_set_ifindex(neigh, if_index);
    rtnl_neigh_set_dst(neigh, dst_addr);
    rtnl_neigh_set_state(neigh, rtnl_neigh_str2state("permanent"));

    // Check to see if the neighbor already exists
    fillLinkCache();
    fillNeighborCache();
    struct rtnl_neigh *neigh_in_cache = rtnl_neigh_get(neighborCache_, if_index, dst_addr);


    VLOG(2) << "Checking to see if neigh already exists";
    // Build the netlink message and send it to the kernel, the operation will
    // block until the operation has been completed.
    
    if (neigh_in_cache != NULL) {
        if(rtnl_neigh_get_state(neigh_in_cache) == NUD_FAILED) {
            VLOG(2) << "Current state is NUD_FAILED, initiating create";
            addop = rtnl_neigh_add(sock_, neigh, NLM_F_CREATE);
        } else {
            VLOG(2) << "Replace action for existing neighbor";
            addop = rtnl_neigh_add(sock_, neigh, NLM_F_REPLACE);
        }
    } else {
        VLOG(2) << "Create action for new neighbor";
        addop = rtnl_neigh_add(sock_, neigh, NLM_F_CREATE);
    }
    // Free the memory
    nl_addr_put(dst_addr);
    rtnl_neigh_put(neigh);

    return addop;
}


int
NetlinkSubscriber::delIpv6Neighbor(std::string ifname,
                                   std::string neigh_dst_addr) {
    // Allocate an empty neighbour handle to be filled out with the attributes
    // of the new neighbour.
    struct rtnl_neigh *neigh = rtnl_neigh_alloc();

    struct nl_addr* dst_addr;

    if (nl_addr_parse(neigh_dst_addr.c_str(), AF_INET6, &dst_addr) < 0) {
        return -1;
    }


    auto if_index = ifNameToIndex(linkCache_, ifname);
    // Fill out the attributes of the new neighbour
    rtnl_neigh_set_ifindex(neigh, if_index);
    rtnl_neigh_set_dst(neigh, dst_addr);

    // Build the netlink message and send it to the kernel, the operation will
    // block until the operation has been completed.
    auto delop = rtnl_neigh_delete(sock_, neigh, 0);

    // Free the memory
    nl_addr_put(dst_addr);
    rtnl_neigh_put(neigh);

    return delop;
}

// Invoked from libnl data processing callback whenver there
// is data on the socket
// Our mutex should be held
void
NetlinkSubscriber::handleLinkEvent(
    nl_object* obj, bool deleted, bool runHandler) noexcept {
  try {
    auto linkEntry = buildLink(obj, deleted);
    if (!linkEntry) {
      return;
    }
    VLOG(2) << "Link Event: " << linkEntry->ifName << "(" << linkEntry->ifIndex
            << ") " << (linkEntry->isUp ? "up" : "down");
    if (runHandler) {
      handler_->linkEventFunc(*linkEntry);
    }
    if (!linkEntry->isUp) {
      removeNeighborCacheEntries(linkEntry->ifName);
    }
    links_[linkEntry->ifName].isUp = linkEntry->isUp;
    links_[linkEntry->ifName].ifIndex = linkEntry->ifIndex;
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
    VLOG(3) << "Neigbbor event: " << neighborEntry->ifName
            << " dest: " << neighborEntry->destination
            << " linkAddr: " << neighborEntry->linkAddress
            << (neighborEntry->isReachable ? " Reachable" : " Unreachable");
    if (runHandler) {
      handler_->neighborEventFunc(*neighborEntry);
    }
    auto neighborKey =
        std::make_pair(neighborEntry->ifName, neighborEntry->destination);
    if (neighborEntry->isReachable) {
      neighbors_.emplace(neighborKey, std::move(neighborEntry->linkAddress));
    } else {
      neighbors_.erase(neighborKey);
    }
  } catch (std::exception const& e) {
    LOG(ERROR) << "Error building neighbor entry/invoking registered handler: "
               << folly::exceptionStr(e);
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
    VLOG(3) << "Address event: " << addrEntry->ifName
            << ", address: " << addrEntry->network.first.str()
            << (addrEntry->isValid ? " Valid" : " Invalid");

    if (runHandler) {
      handler_->addrEventFunc(*addrEntry);
    }

    if (addrEntry->isValid) {
      links_[addrEntry->ifName].networks.insert(addrEntry->network);
    } else {
      auto it = links_.find(addrEntry->ifName);
      if (it != links_.end()) {
        it->second.networks.erase(addrEntry->network);
      }
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
NetlinkSubscriber::fillLinkCache() {
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
NetlinkSubscriber::fillNeighborCache() {
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

// Data is ready on socket, request a read. This will invoke our registered
// handler which in turn will retrieve the user stored handler and
// invoke it
void
NetlinkSubscriber::dataReady() {
  std::lock_guard<std::mutex> lock(netlinkMutex_);
  int err = nl_cache_mngr_data_ready(cacheManager_);
  if (err < 0) {
    throw NetlinkException(folly::sformat(
        "Failed to read ready data from cache manager. Error: {}",
        nl_geterror(err)));
  }
}

// Process any outstanding events on the socket and let libnl
// update all registered caches
// If we have no outstanding events, this call should be harmless
void
NetlinkSubscriber::updateFromKernelCaches() {
  try {
    dataReady();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Error processing data on socket: " << folly::exceptionStr(e);
    return;
  }
}

} // namespace openr
