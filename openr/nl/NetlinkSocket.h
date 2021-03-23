/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqEventLoop.h>
#include <folly/ConcurrentBitSet.h>
#include <folly/IPAddress.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <openr/nl/NetlinkProtocolSocket.h>

namespace openr::fbnl {

using EventVariant = std::variant<Neighbor, IfAddress, Link>;

struct PrefixCmp {
  bool
  operator()(
      const folly::CIDRNetwork& lhs, const folly::CIDRNetwork& rhs) const {
    if (lhs.first != rhs.first) {
      return lhs.first < rhs.first;
    } else {
      return lhs.second < rhs.second;
    }
  }
};

/**
 * A general netlink class provides APIs that interact with kernel on route
 * programming, iface/address management, iface/route/address monitoring,
 * link/addr/neighbor/route events subscribe/Unsubscribe
 *
 * For testability:
 *   Making the public interfaces VIRTUAL to make it works with GMock framework

 * For events subscription:
 *   User can use NetlinkSocket::EventsHandler to implement events handlers then
 *   using sub/unsub APIs to control events subscription
 *
 * A ZmqEventLoop is provided which the implementation uses to register
 * socket fds. Caller is responsible for running the zmq event loop.
 *
 * For concurrency model:
 *   User can create the object in main thread. Internally we register fds with
 *   the provided zmq event loop. So user must make sure that the event loop is
 *   NOT already running at time of creating this object.
 *
 *   The getAllLinks() and getAllReachableNeighbors() API can be called from
 *   main thread. They should however not be called from within the registered
 *   handlers in the EventHandler object provided below (there should be no good
 *   reason to anyway) Internally, the user provided Handler funcs and get*()
 *   methods both update the same cache, which we protect by
 *   serializing calls into a single eventloop.
 */
class NetlinkSocket {
 public:
  // A simple collection of handlers invoked on relevant netlink events. If
  // caller is not interested in a handler, it can simply not override it
  class EventsHandler {
   public:
    EventsHandler() = default;
    virtual ~EventsHandler() = default;

    // Callback invoked by NetlinkSocket when registered event happens
    void
    handleEvent(const std::string& ifName, const EventVariant& event) noexcept {
      std::visit(EventVisitor(ifName, this), event);
    }

    virtual void
    linkEventFunc(
        const std::string& /* ifName */,
        const openr::fbnl::Link& /* linkEntry */) noexcept {
      LOG(FATAL) << "linkEventFunc is not implemented";
    }

    virtual void
    neighborEventFunc(
        const std::string& /* ifName */,
        const openr::fbnl::Neighbor& /* neighborEntry */) noexcept {
      LOG(FATAL) << "neighborEventFunc is not implemented";
    }

    virtual void
    addrEventFunc(
        const std::string& /* ifName */,
        const openr::fbnl::IfAddress& /* addrEntry */) noexcept {
      LOG(FATAL) << "addrEventFunc is not implemented";
    }

   private:
    EventsHandler(const EventsHandler&) = delete;
    EventsHandler& operator=(const EventsHandler&) = delete;
  };

  class NeighborUpdate {
   public:
    NeighborUpdate() = default;
    void
    addNeighbor(std::string add) {
      added_.push_back(add);
    }
    void
    delNeighbor(std::string del) {
      removed_.push_back(del);
    }
    void
    delNeighbors(const std::vector<std::string>& del) {
      removed_.insert(removed_.end(), del.begin(), del.end());
    }
    std::vector<std::string>
    getAddedNeighbor() {
      return added_;
    }
    std::vector<std::string>
    getRemovedNeighbor() {
      return removed_;
    }

   private:
    std::vector<std::string> added_;
    std::vector<std::string> removed_;
  };

  struct EventVisitor {
    std::string linkName;
    EventsHandler* eventHandler;
    EventVisitor(const std::string& ifName, EventsHandler* handler)
        : linkName(ifName), eventHandler(handler) {}

    void
    operator()(openr::fbnl::IfAddress const& addr) const {
      eventHandler->addrEventFunc(linkName, addr);
    }

    void
    operator()(openr::fbnl::Neighbor const& neigh) const {
      eventHandler->neighborEventFunc(linkName, neigh);
    }

    void
    operator()(openr::fbnl::Link const& link) const {
      eventHandler->linkEventFunc(linkName, link);
    }
  };

  explicit NetlinkSocket(
      fbzmq::ZmqEventLoop* evl,
      EventsHandler* handler = nullptr,
      std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlSock = nullptr);

  virtual ~NetlinkSocket();

  /**
   * Add unicast routes to route table
   *
   * Add unicast route to kernel, see RouteBuilder::buildUnicastRoute()
   * If adding multipath nextHops for the same prefix at different times,
   * always provide unique nextHops and not cumulative list.
   * Currently we do not enforce checks from local cache,
   * but kernel will reject the request
   * @throws fbnl::NlException
   *
   * @throws fbnl::NlException if the route already existed
   */
  virtual folly::Future<folly::Unit> addRoute(Route route);

  /**
   * Add MPLS label route, nexthop semantics is same as route nexthop
   */
  virtual folly::Future<folly::Unit> addMplsRoute(Route route);

  /**
   * Delete unicast routes from route table
   * This will delete route according to destination, nexthops. You must set
   * exactly the same destination and nexthops as in route table.
   * For convience, one can just set destination, this will delete all nextHops
   * accociated with it.
   * @throws fbnl::NlException
   */
  virtual folly::Future<folly::Unit> delRoute(Route route);

  /**
   * delete MPLS route. Only label is needed to delete the label route
   */
  virtual folly::Future<folly::Unit> delMplsRoute(Route route);

  /**
   * Sync route table in kernel with given route table
   * Delete routes that not in the 'newRouteDb' but in kernel
   * Add/Update routes in 'newRouteDb'
   * Basically when there's mismatch between backend kernel and route table in
   * application, we sync kernel routing table with given data source
   * @throws fbnl::NlException
   */
  virtual folly::Future<folly::Unit> syncUnicastRoutes(
      uint8_t protocolId, NlUnicastRoutes newRouteDb);

  /**
   * Sync MPLS label routes. Delete label routes not in 'MplsRouteDb' and
   * add the routes not present in kernel
   */
  virtual folly::Future<folly::Unit> syncMplsRoutes(
      uint8_t protocolId, NlMplsRoutes newMplsRouteDb);

  /**
   * Get cached unicast routing by protocol ID
   * @throws fbnl::NlException
   */
  virtual folly::Future<NlUnicastRoutes> getCachedUnicastRoutes(
      uint8_t protocolId) const;

  /**
   * Get cached MPLS routes by protocol ID
   * @throws fbnl::NlException
   */
  virtual folly::Future<NlMplsRoutes> getCachedMplsRoutes(
      uint8_t protocolId) const;

  /**
   * Get number of all cached routes
   * @throws fbnl::NlException
   */
  virtual folly::Future<int64_t> getRouteCount() const;

  /**
   * Get number of all cached MPLS routes
   * @throws fbnl::NlException
   */
  virtual folly::Future<int64_t> getMplsRouteCount() const;

  /**
   * Add Interface address e.g. ip addr add 192.168.1.1/24 dev em1
   * @throws fbnl::NlException
   */
  virtual folly::Future<folly::Unit> addIfAddress(fbnl::IfAddress ifAddr);

  /**
   * Delete Interface address e.g.
   * -- ip addr del 192.168.1.1/24 dev em1
   *
   * Prefix, ifIndex are mandatory, the specific address and
   * interface tuple will be deleted
   */
  virtual folly::Future<folly::Unit> delIfAddress(fbnl::IfAddress ifAddr);

  /**
   * Sync addrs on the specific iface, the iface in addrs should be the same,
   * otherwiese the method will throw fbnl::NlException.
   * There are two steps to sync address
   * 1. Add 'addrs' to the iface
   * 2. Delete addresses according to ifIndex, family, scope
   * 'family' and 'scope' are used to give more specific sync conditions
   * If not set, they are not considered when deleting addresses.
   * If set, only addresses with 'family' and 'scope' will be deleted
   * before sync
   *
   * When addrs is empty, this will flush all the addresses on the iface
   * according to family and scope. Leaving out family and scope will
   * result in all addresses of the specified address interface tuple to
   * be deleted.
   */
  virtual folly::Future<folly::Unit> syncIfAddress(
      int ifIndex, std::vector<fbnl::IfAddress> addrs, int family, int scope);

  /**
   * Get iface address list on ifIndex filtered on family and scope
   */
  virtual folly::Future<std::vector<fbnl::IfAddress>> getIfAddrs(
      int ifIndex, int family, int scope);

  /**
   * Get interface index from name
   * -1 means no such interface
   * @throws fbnl::NlException
   */
  virtual folly::Future<int> getIfIndex(const std::string& ifName);

  /**
   * get loopback interface index
   */
  virtual folly::Future<std::optional<int>> getLoopbackIfIndex();

  /**
   * Get interface name form index
   * @throws fbnl::NlException
   */
  virtual folly::Future<std::string> getIfName(int ifIndex) const;

  /**
   * Get all links entries
   * This will invoke subscriber methods even if eventLoop is not yet
   * running. Subscriber method will be invoked in calling thread context
   * @throws fbnl::NlException
   */
  virtual folly::Future<NlLinks> getAllLinks();

  /**
   * Get all the neighbor entries
   * This can be used to obtain link addresses of nextHops
   * This will invoke subscriber methods even if eventLoop is not yet
   * running. Subscriber method will be invoked in calling thread context
   */
  virtual folly::Future<NlNeighbors> getAllReachableNeighbors();

  // get all routes from kernel
  std::vector<fbnl::Route> getAllRoutes() const;

  /**
   * Subscribe specific event
   * No effect for invalid event types
   * NOTE: By default NetlinkSocket subscribes NO event
   */
  void subscribeEvent(NetlinkEventType event);

  /**
   * Unsubscribe specific event
   * No effect for invalid event types
   */
  void unsubscribeEvent(NetlinkEventType event);

  // Subscribe all supported events
  void subscribeAllEvents();

  // Unsubscribe all events
  void unsubscribeAllEvents();

  void setEventHandler(EventsHandler* handler);

  // Expose pointer to underlying protocol socket
  NetlinkProtocolSocket*
  getProtocolSocket() {
    return nlSock_.get();
  }

 private:
  void doHandleRouteEvent(
      Route route, bool runHandler, bool updateUnicastRoute) noexcept;

  void doHandleLinkEvent(Link link, bool runHandler) noexcept;

  void doHandleAddrEvent(IfAddress ifAddr, bool runHandler) noexcept;

  void doHandleNeighborEvent(Neighbor neighbor, bool runHandler) noexcept;

  void doUpdateRouteCache(Route route, bool updateUnicastRoute = false);

  void doAddUpdateUnicastRoute(Route route);

  void doDeleteUnicastRoute(Route route);

  void doAddUpdateMplsRoute(Route route);

  void doDeleteMplsRoute(Route route);

  void doSyncUnicastRoutes(uint8_t protocolId, NlUnicastRoutes syncDb);

  void checkUnicastRoute(const Route& route);

  void doSyncIfAddress(
      int ifIndex, std::vector<fbnl::IfAddress> addrs, int family, int scope);

  void doGetIfAddrs(
      int ifIndex, int family, int scope, std::vector<fbnl::IfAddress>& addrs);

  void removeNeighborCacheEntries(const std::string& ifName);

  void updateLinkCache();

  void updateAddrCache();

  void updateNeighborCache();

  void updateRouteCache();

 private:
  fbzmq::ZmqEventLoop* evl_{nullptr};

  /**
   * Local cache. We do not use this to enforce any checks
   * for incoming requests. Merely an optimization for get cached routes
   */
  NlUnicastRoutesDb unicastRoutesCache_;

  /**
   * MPLS label route cache
   */
  NlMplsRoutesDb mplsRoutesCache_;

  EventsHandler* handler_{nullptr};

  std::optional<int> loopbackIfIndex_;

  /**
   * We keep an internal cache of Neighbor and Link entries
   * These are used in the getAllLinks/getAllReachableNeighbors methods
   */
  NlNeighbors neighbors_{};
  NlLinks links_{};

  // Indicating to run which event type's handler
  folly::ConcurrentBitSet<MAX_EVENT_TYPE> eventFlags_;

  std::unique_ptr<openr::fbnl::NetlinkProtocolSocket> nlSock_{nullptr};
};

} // namespace openr::fbnl
