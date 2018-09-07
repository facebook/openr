/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <map>
#include <unordered_map>

#include <fbzmq/async/ZmqEventLoop.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/AtomicBitSet.h>

#include <openr/nl/NetlinkTypes.h>

extern "C" {
#include <linux/if.h>
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include <netlink/socket.h>
}

namespace openr {


// Link Object Helpers
// map of links and their attributes
// attributes can be expanded to include context (state) in future
struct LinkAttributes {
  bool isUp{false};
  int ifIndex{0};
  std::unordered_set<folly::CIDRNetwork> networks;
};
// keyed by link name
using Links = std::unordered_map<std::string, LinkAttributes>;

// helper object to convert libnl objects
struct LinkEntry {
  LinkEntry(std::string ifName, bool isUp, int ifIndex)
      : ifName(ifName), isUp(isUp), ifIndex(ifIndex) {}
  const std::string ifName{};
  const bool isUp{false};
  const int ifIndex{0};
};

// Neighbor Object Helpers
// Map of neighbors that are reachable
// link name, destination IP and link Address
using Neighbors = std::
    unordered_map<std::pair<std::string, folly::IPAddress>, folly::MacAddress>;

struct AddrEntry {
  std::string ifName;
  folly::CIDRNetwork network;
  bool isValid;
};

// helper object to report status of neighbor as it changes
// linkAddress can be ignored for unreachable neighbors
struct NeighborEntry {
  NeighborEntry(
      std::string ifName,
      folly::IPAddress destination,
      folly::MacAddress linkAddress,
      bool isReachable)
      : ifName(ifName),
        destination(destination),
        linkAddress(linkAddress),
        isReachable(isReachable) {}

  const std::string ifName{};
  const folly::IPAddress destination{"::0"};
  const folly::MacAddress linkAddress{"00:00:00:00:00:00"};
  const bool isReachable{false};
};

// Route helper type
struct RouteEntry {
  RouteEntry(
    const folly::CIDRNetwork& prefix,
    uint32_t scope, uint32_t table,
    uint32_t proto, uint32_t type, bool isDeleted)
    : scope(scope),
      table(table),
      proto(proto),
      type(type),
      isDeleted(isDeleted),
      prefix(prefix) {}

  const uint32_t scope{RT_SCOPE_NOWHERE};
  const uint32_t table{RT_TABLE_UNSPEC};
  const uint32_t proto{99};
  const uint32_t type{RTN_UNICAST};
  const bool isDeleted{false};
  const folly::CIDRNetwork prefix;
  std::unordered_set<std::pair<std::string, folly::IPAddress>> nexthops;
};

// A simple wrapper over Netlink Socket for Subscribing to events and
// getting cached state.
// Contains all netlink details. We throw execptions for anything gone wrong
//
// User can use NetlinkSubscriber::Handler to get notifications or simply
// request
// dump of interested objects via the get* API
//
// A ZmqEventLoop is provided which the implementation uses to register
// socket fds. Caller is responsible for running the zmq event loop.
//
// Note on concurrency model :
// -------------------------
// User can create the object in main thread. Internally we register fds with
// the provided zmq event loop. So user must make sure that the event loop is
// not already running at time of creating this object.
//
// The getAllLinks() and getAllReachableNeighbors() API can be called from main
// thread
// They should however not be called from within the registered handlers in the
// Handler object provided below (there should be no good reason to anyway)
// Internally, the user provided Handler funcs and get*() methods both update
// the same internal libnl cache, which we protect by serializing calls into a
// sigle eventloop.
//
// Further, getAllLinks() / getAllReachableNeighbors() will internally request
// refill of libnl caches. This will trigger user regiseterd Handler func to be
// invoked in the context of the calling thread (main thread for example)
// User should therefore not expect to have registered handler funcs be invoked
// only in the context of zmq event loop thread
class NetlinkSubscriber final {
 public:

  // A simple collection of handlers invoked on relevant events
  // This object is passed to NetlinkSubscriber
  // If caller is not interested in a handler, it can simply not override it
  class Handler {
   public:
    Handler() = default;
    virtual ~Handler() = default;

    virtual void
    linkEventFunc(const LinkEntry& linkEntry) {
      VLOG(3) << "Link : " << linkEntry.ifName
              << (linkEntry.isUp ? " UP" : " DOWN");
    }

    virtual void
    neighborEventFunc(const NeighborEntry& neighborEntry) {
      VLOG(3)
          << "Neighbor entry: " << neighborEntry.ifName << " : "
          << neighborEntry.destination.str() << " -> "
          << neighborEntry.linkAddress.toString()
          << (neighborEntry.isReachable ? " : Reachable" : " : Unreachable");
    }

    virtual void
    addrEventFunc(const AddrEntry& addrEntry) {
      VLOG(3)
          << "Address: " << folly::IPAddress::networkToString(addrEntry.network)
          << " on link: " << addrEntry.ifName
          << (addrEntry.isValid ? " ADDED" : " DELETED");
    }

    virtual void
    routeEventFunc(const RouteEntry& routeEntry) {
      VLOG(3) << "Route entry: "
              << folly::IPAddress::networkToString(routeEntry.prefix)
              << (routeEntry.isDeleted ? " Deleted " : " Added");
    }

   private:
    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
  };

  NetlinkSubscriber(fbzmq::ZmqEventLoop* zmqLoop, Handler* subscriberHandler);
  ~NetlinkSubscriber();

  // Can be called from main thread
  // Get all links entries
  // Internally uses mutex to protect against event processing in
  // zmq event loop thread
  // This will invoke subscriber methods even if eventLoop is not yet
  // running. Subscriber method will be invoked in calling thread context
  Links getAllLinks();

  // Can be called from main thread
  // Get all the neighbor entries
  // This can be used to obtain link addresses of nextHops
  // Internally uses mutex to protect against event processing in
  // zmq event loop thread
  // This will invoke subscriber methods even if eventLoop is not yet
  // running. Subscriber method will be invoked in calling thread context
  Neighbors getAllReachableNeighbors();

  // Subscribe specific event
  // No effect for invalid event types
  void subscribeEvent(fbnl::NetlinkEventType event);

  // Unsubscribe specific event
  // No effect for invalid event types
  void unsubscribeEvent(fbnl::NetlinkEventType event);

  // Subscribe all supported events
  void subscribeAllEvents();

  // Unsubscribe all events
  void unsubscribeAllEvents();

 private:
  NetlinkSubscriber(const NetlinkSubscriber&) = delete;
  NetlinkSubscriber& operator=(const NetlinkSubscriber&) = delete;

  // This is the callback we pass into libnl when data is ready on the socket
  // The opaque data will contain the user registered NetlinkSubscriber
  // These are static to match C function vector prototype
  static void linkEventFunc(
      struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept;

  static void neighborEventFunc(
      struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept;

  static void addrEventFunc(
      struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept;

  static void routeEventFunc(
    struct nl_cache*, struct nl_object* obj, int action, void* data) noexcept;

  void handleLinkEvent(nl_object* obj, bool deleted, bool runHandler) noexcept;
  void handleNeighborEvent(
      nl_object* obj, bool deleted, bool runHandler) noexcept;
  void handleAddrEvent(nl_object* obj, bool deleted, bool runHandler) noexcept;
  void handleRouteEvent(nl_object* obj, bool deleted, bool runHandler) noexcept;

  // Helper methods to do full update of link cache from kernel
  void updateLinkAddrCache();
  void updateNeighborCache();

  // If a link went down, remove all associated neighbor entries
  void removeNeighborCacheEntries(const std::string& ifName);

  // Netlink scokets to interact with linux kernel. We deliberately use two
  // sockets as it is a recommended way instead of multiplexing things over
  // single socket. We have been seeing issues with multiplexing things (like
  // Object Busy, Link Cache not updating etc.)
  //
  // subNlSock_ => Is a read only socket which subscribes to multicast groups
  //                 for links, addrs and neighbor notifications.
  // reqNlSock_ => Is a req/rep socket that is used to request full snapshot of
  //               links, addrs and neighbor entries and update local cache.
  struct nl_sock* subNlSock_{nullptr};
  struct nl_sock* reqNlSock_{nullptr};

  // libnl uses the cacheManager to manages all caches
  // This is needed to subscribe for events
  struct nl_cache_mngr* cacheManager_{nullptr};

  // Caches are created internally by cacheManager
  struct nl_cache* neighborCache_{nullptr};
  struct nl_cache* linkCache_{nullptr};
  struct nl_cache* addrCache_{nullptr};
  struct nl_cache* routeCache_{nullptr};

  fbzmq::ZmqEventLoop* zmqLoop_{nullptr};
  Handler* handler_{nullptr};

  // We keep an internal cache of Neighbor and Link entries
  // These are used in the get* methods
  Neighbors neighbors_{};
  Links links_{};

  // Indicating to run which event type's handler
  folly::AtomicBitSet<fbnl::MAX_EVENT_TYPE> eventFlags_;
};
} // namespace openr
