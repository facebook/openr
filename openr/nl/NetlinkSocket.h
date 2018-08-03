/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/variant.hpp>

#include "NetlinkException.h"
#include "NetlinkTypes.h"

#include <fbzmq/async/ZmqEventLoop.h>

#include <folly/futures/Future.h>

namespace openr {
namespace fbnl {

using EventVariant = boost::variant<Route, Neighbor, IfAddress, Link>;

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
 *   methods both update the same internal libnl cache, which we protect by
 *   serializing calls into a sigle eventloop.
 *
 *   Further, getAllLinks() / getAllReachableNeighbors() will internally request
 *   refill of libnl caches. This will trigger user regiseterd Handler func to
 *   be invoked in the context of the calling thread (main thread for example)
 *   User should therefore not expect to have registered handler funcs be
 *   invoked only in the context of zmq event loop thread
 */
class NetlinkSocket {
 public:
   enum NetlinkEventType {
     LINK_EVENT = 0,
     NEIGH_EVENT,
     ADDR_EVENT,
     MAX_EVENT_TYPE // sentinel
   };

   // A simple collection of handlers invoked on relevant events
   // This object is passed to NetlinkSubscriber
   // If caller is not interested in a handler, it can simply not override it
   class EventsHandler {
    public:
     EventsHandler() = default;
     virtual ~EventsHandler() = default;

     // Callback invoked by NetlinkSocket when registered event happens
     void handleEvent(int action, const EventVariant& event) {
       boost::apply_visitor(EventVisitor(action, this), event);
     }

     virtual void linkEventFunc(int action, const Link& linkEntry) {
       VLOG(3) << "LinkEventFunc";
     }

     virtual void neighborEventFunc(int action, const Neighbor& neighborEntry) {
       VLOG(3) << "NeighborEventFucn";
     }

     virtual void addrEventFunc(int action, const IfAddress& addrEntry) {
       VLOG(3) << "AddrEventFunc";
     }

     virtual void routeEventFunc(int action, const Route& routeEntry) {
       VLOG(3) << "RouteEventFunc";
     }

    private:
     EventsHandler(const EventsHandler&) = delete;
     EventsHandler& operator=(const EventsHandler&) = delete;
   };

   struct EventVisitor {
     EventsHandler* eventHandler;
     int32_t eventAction; // NL_ACT_DEL, NL_ACT_NEW
     EventVisitor(int action, EventsHandler* handler);

     void operator()(Route const& route) {
       eventHandler->routeEventFunc(eventAction, route);
     }

     void operator()(IfAddress const& addr) {
       eventHandler->addrEventFunc(eventAction, addr);
     }

     void operator()(Neighbor const& neigh) {
       eventHandler->neighborEventFunc(eventAction, neigh);
     }

     void operator()(Link const& link) {
       eventHandler->linkEventFunc(eventAction, link);
     }
   };

   explicit NetlinkSocket(fbzmq::ZmqEventLoop* evl, EventsHandler);
   virtual ~NetlinkSocket();

   /**
    * Add unicast/multicast/link routes to route table
    *
    * Add unicast route to kernel, see RouteBuilder::buildUnicastRoute()
    * If adding multipath nextHops for the same prefix at different times,
    * always provide unique nextHops and not cumulative list.
    * Currently we do not enforce checks from local cache,
    * but kernel will reject the request
    * @throws NetlinkException
    *
    * Add multicast route, see RouteBuilder::buildMulticastRoute()
    * @throws NetlinkException if the route already existed
    */
   virtual folly::Future<folly::Unit> addRoute(Route route);

   /**
    * Delete unicast/multicast routes from route table
    * This will delete route according to destination, nexthops. You must set
    * exactly the same destination and nexthops as in route table.
    * For convience, one can just set destination, this will delete all nextHops
    * accociated with it.
    * @throws NetlinkException
    */
   virtual folly::Future<folly::Unit> delRoute(Route route);

   /**
    * Sync route table in kernel with given route table
    * Delete routes that not in the 'newRouteDb' but in kernel
    * Add/Update routes in 'newRouteDb'
    * Basically when there's mismatch between backend kernel and route table in
    * application, we sync kernel routing table with given data source
    * @throws NetlinkException
    */
   virtual folly::Future<folly::Unit>
   syncUnicastRoutes(NlUnicastRoutes newRouteDb);

   /**
    * Delete routes that not in the 'newRouteDb' but in kernel
    * Add/Update routes in 'newRouteDb'
    * @throws NetlinkException
    */
   virtual folly::Future<folly::Unit>
   syncLinkRoutes(NlLinkRoutes newRouteDb);

   /**
    * Get cached unicast routing by protocol ID
    * @throws NetlinkException
    */
   virtual folly::Future<NlUnicastRoutes>
   getCachedUnicastRoutes(uint8_t protocolId) const;

   /**
    * Get cached multicast routing by protocol ID
    * @throws NetlinkException
    */
   virtual folly::Future<NlMulticastRoutes>
   getCachedMulticastRoutes(uint8_t protocolId) const;

   /**
    * Get cached link route by protocol ID
    * @throws NetlinkException
    */
   virtual folly::Future<NlLinkRoutes>
   getCachedLinkRoutes(uint8_t protocolId) const;

   /**
    * Get number of all cached routes
    * @throws NetlinkException
    */
   virtual folly::Future<int64_t> getRouteCount() const;

   /**
    * Add Interface address e.g. ip addr add 192.168.1.1/24 dev em1
    * @throws NetlinkException
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
     * otherwiese the method will throw NetlinkException.
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
      int ifIndex,
      std::vector<fbnl::IfAddress> addrs,
      int family, int scope);

   /**
    * Get iface address list according
    */
   virtual folly::Future<std::vector<fbnl::IfAddress>> getIfAddrs(
        int ifIndex, int family, int scope);

   /**
    * Get interface index from name
    * 0 means no such interface
    * @throws NetlinkException
    */
   virtual folly::Future<int> getIfIndex(const std::string& ifName) const;

   /**
    * Get interface name form index
    * @throws NetlinkException
    */
   virtual folly::Future<std::string> getIfName(int ifIndex) const;

   /**
    * Get all links entries
    * This will invoke subscriber methods even if eventLoop is not yet
    * running. Subscriber method will be invoked in calling thread context
    * @throws NetlinkException
    */
   virtual folly::Future<NlLinks> getAllLinks();

   /**
    * Get all the neighbor entries
    * This can be used to obtain link addresses of nextHops
    * This will invoke subscriber methods even if eventLoop is not yet
    * running. Subscriber method will be invoked in calling thread context
    */
   virtual folly::Future<NlNeighbors> getAllReachableNeighbors();

   /**
    * Subscribe specific event
    * No effect for invalid event types
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

 private:
   /**
    * Netlink scokets to interact with linux kernel. We deliberately use two
    * sockets as it is a recommended way instead of multiplexing things over
    * single socket. We have been seeing issues with multiplexing things (like
    * Object Busy, Link Cache not updating etc.)
    *
    * subSock_ => Is a read only socket which subscribes to multicast groups
                   for links, addrs and neighbor notifications.
    * reqSock_ => Is a req/rep socket that is used to request full snapshot of
                 links, addrs and neighbor entries and update local cache.
    */
   struct nl_sock* subSock_{nullptr};
   struct nl_sock* reqSock_{nullptr};

   /**
    * libnl uses the cacheManager to manages all caches
    * This is needed to subscribe for events
    */
   struct nl_cache_mngr* cacheManager_{nullptr};

   // Caches are created internally by cacheManager
   struct nl_cache* neighborCache_{nullptr};
   struct nl_cache* linkCache_{nullptr};
   struct nl_cache* addrCache_{nullptr};
   struct nl_cache* routeCacheV4_{nullptr};
   struct nl_cache* routeCacheV6_{nullptr};

   fbzmq::ZmqEventLoop* evl_{nullptr};

   /**
    * Local cache. We do not use this to enforce any checks
    * for incoming requests. Merely an optimization for get cached routes
    */
   NlUnicastRoutesDb unicastRoutesCache_;

   // Check against redundant multicast routes
   NlMulticastRoutesDb mcastRoutesCache_;

   NlLinkRoutesDb linkRoutesCache_;

   EventsHandler* handler_{nullptr};

   /**
    * We keep an internal cache of Neighbor and Link entries
    * These are used in the get* methods
    */
   NlNeighbors neighbors_{};
   NlLinks links_{};

   // Indicating to run which event type's handler
   folly::AtomicBitSet<MAX_EVENT_TYPE> eventFlags_;
};

} // namespace fbnl
} // namespace openr
