/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <folly/Optional.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>

extern "C" {
#include <linux/if.h>
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/route.h>
#include <netlink/route/neighbour.h>
#include <netlink/socket.h>
}

namespace openr {
namespace fbnl {

const uint8_t DEFAULT_PROTOCOL_ID = 99;

enum NetlinkEventType {
  LINK_EVENT = 0,
  NEIGH_EVENT,
  ADDR_EVENT,
  ROUTE_EVENT,
  MAX_EVENT_TYPE // sentinel
};

class NextHop;
class NextHopBuilder final {
 public:
   NextHopBuilder() {}
   ~NextHopBuilder() {}

   NextHop buildFromObject(struct rtnl_nexthop* obj) const;
   NextHop build() const;

   void reset();

   NextHopBuilder& setIfIndex(int ifIndex);

   NextHopBuilder& setGateway(const folly::IPAddress& gateway);

   NextHopBuilder& setWeight(uint8_t weight);

   folly::Optional<int> getIfIndex() const;

   folly::Optional<folly::IPAddress> getGateway() const;

   folly::Optional<uint8_t> getWeight() const;

 private:
   folly::Optional<int> ifIndex_;
   folly::Optional<folly::IPAddress> gateway_;
   folly::Optional<uint8_t> weight_;
};

// Wrapper class for rtnl_nexthop
class NextHop final {
 public:
   explicit NextHop(const NextHopBuilder& builder);

   folly::Optional<int> getIfIndex() const;

   folly::Optional<folly::IPAddress> getGateway() const;

   folly::Optional<uint8_t> getWeight() const;

   /**
    * This method Will construct rtnl_nexthop object on the first time call,
    * then will return the same object pointer. It will just return the pointer.
    * Usually rtnl_nexthop object will be added to rtnl_route object which Will
    * manage the nextHop object, caller don't need to free it manually.
    * Otherwise, caller should use release call to release the object
    */
   struct rtnl_nexthop* fromNetlinkNextHop() const;

   void release();

   std::string str() const;

  private:
   void init();

   // Nexthop build helper
   struct rtnl_nexthop* buildNextHopInternal(
     int ifIdx, const folly::IPAddress& gateway);

   struct rtnl_nexthop* buildNextHopInternal(
     const folly::IPAddress& gateway);

   struct rtnl_nexthop* buildNextHopInternal(int ifIdx);

private:
   folly::Optional<int> ifIndex_;
   folly::Optional<folly::IPAddress> gateway_;
   folly::Optional<uint8_t> weight_;
   struct rtnl_nexthop* nextHop_{nullptr};
};


bool operator==(const NextHop& lhs, const NextHop& rhs);

struct NextHopHash {
  size_t operator() (const openr::fbnl::NextHop& nh) const;
};

using NextHopSet = std::unordered_set<NextHop, NextHopHash>;
/**
 * Values for core fields
 * ============================
 * 'routeTable_':
 * RT_TABLE_MAIN (default)
 * RT_TABLE_UNSPEC
 * RT_TABLE_DEFAULT
 * RT_TABLE_LOCAL
 * RT_TABLE_MAX
 * ============================
 * 'scope_':
 * RT_SCOPE_UNIVERSE (default)
 * RT_SCOPE_SITE
 * RT_SCOPE_LINK
 * RT_SCOPE_HOST
 * RT_SCOPE_NOWHERE
 * ============================
 * 'type_':
 * RTN_UNICAST (default)
 * RTN_MULTICAST
 * ============================
 * 'protocolId_'
 * 99 (default)
 * ============================
 * 'flags_':
 * RTM_F_NOTIFY
 * RTM_F_CLONED
 */
class Route;
class RouteBuilder {
 public:
  // Default values for are set as aforementioned
  RouteBuilder() {}
  ~RouteBuilder() {}

  /**
   * Build route (default: unicast)
   * @required parameter:
   * ProtocolId, Destination, Nexthop
   * @throw NetlinkException on failed
   */
  Route buildRoute() const;

  Route buildFromObject(struct rtnl_route* obj);

  RouteBuilder& loadFromObject(struct rtnl_route* obj);

  /**
   * Build multicast route
   * @required parameter:
   * ProtocolId, Destination, Iface Name, Iface Index
   * @throw NetlinkException on failed
   */
  Route buildMulticastRoute() const;

  /**
   * Build link route
   * @required parameter:
   * ProtocolId, Destination, Iface Name, Iface Index
   * @throw NetlinkException on failed
   */
  Route buildLinkRoute() const;

  // Required
  RouteBuilder& setDestination(const folly::CIDRNetwork& dst);

  const folly::CIDRNetwork& getDestination() const;

  // Required, default RTN_UNICAST
  RouteBuilder& setType(uint8_t type = RTN_UNICAST);

  uint8_t getType() const;

  // Required, default RT_TABLE_MAIN
  RouteBuilder& setRouteTable(uint8_t routeTable = RT_TABLE_MAIN);

  uint8_t getRouteTable() const;

  // Required, default 99
  RouteBuilder& setProtocolId(uint8_t protocolId = DEFAULT_PROTOCOL_ID);

  uint8_t getProtocolId() const;

  // Required, default RT_SCOPE_UNIVERSE
  RouteBuilder& setScope(uint8_t scope = RT_SCOPE_UNIVERSE);

  RouteBuilder& setValid(bool isValid);

  bool isValid() const;

  uint8_t getScope() const;

  RouteBuilder& setFlags(uint32_t flags);

  folly::Optional<uint32_t> getFlags() const;

  RouteBuilder& setPriority(uint32_t priority);

  folly::Optional<uint32_t> getPriority() const;

  RouteBuilder& setTos(uint8_t tos);

  folly::Optional<uint8_t> getTos() const;

  RouteBuilder& addNextHop(const NextHop& nextHop);

  RouteBuilder& setRouteIfName(const std::string& ifName);

  folly::Optional<std::string> getRouteIfName() const;

  // Multicast/Link route only need ifIndex in nexthop
  RouteBuilder& setRouteIfIndex(int ifIndex);

  folly::Optional<int> getRouteIfIndex() const;

  const NextHopSet& getNextHops() const;

  void reset();

 private:
  uint8_t type_{RTN_UNICAST};
  uint8_t routeTable_{RT_TABLE_MAIN};
  uint8_t protocolId_{DEFAULT_PROTOCOL_ID};
  uint8_t scope_{RT_SCOPE_UNIVERSE};
  bool isValid_{false};
  folly::Optional<uint32_t> flags_;
  folly::Optional<uint32_t> priority_;
  folly::Optional<uint8_t> tos_;
  NextHopSet nextHops_;
  folly::CIDRNetwork dst_;
  folly::Optional<int> routeIfIndex_; // for multicast or link route
  folly::Optional<std::string> routeIfName_; // for multicast or linkroute
};

// Wrapper class for rtnl_route
class Route final {
 public:
   explicit Route(const RouteBuilder& builder);

   ~Route();

   Route(Route&&) noexcept;

   Route& operator=(Route&&) noexcept;

   uint8_t getFamily() const;

   const folly::CIDRNetwork& getDestination() const;

   uint8_t getType() const;

   uint8_t getRouteTable() const;

   uint8_t getProtocolId() const;

   uint8_t getScope() const;

   folly::Optional<uint32_t> getFlags() const;

   folly::Optional<uint32_t> getPriority() const;

   folly::Optional<uint8_t> getTos() const;

   const NextHopSet& getNextHops() const;

   bool isValid() const;

   folly::Optional<std::string> getRouteIfName() const;

   /**
    * This method Will construct rtnl_route object on the first time call,
    * then will return the same object pointer. It will just return the pointer
    * without increase the ref count. Caller shouldn't do rtnl_route_put
    * without explicit increase of it's ref count
    */
   struct rtnl_route* fromNetlinkRoute() const;

 private:
   Route& operator=(const Route&);
   Route(const Route&) = default;

   void init();
   struct nl_addr* buildAddrObject(const folly::CIDRNetwork& addr);

   uint8_t type_;
   uint8_t routeTable_;
   uint8_t protocolId_;
   uint8_t scope_;
   bool isValid_{false};
   folly::Optional<uint32_t> flags_;
   folly::Optional<uint32_t> priority_;
   folly::Optional<uint8_t> tos_;
   NextHopSet nextHops_;
   folly::CIDRNetwork dst_;
   folly::Optional<std::string> routeIfName_;
   struct rtnl_route* route_{nullptr};
};

bool operator==(const Route& lhs, const Route& rhs);

class IfAddress;
class IfAddressBuilder final {
 public:
   IfAddressBuilder() {}
   ~IfAddressBuilder() {}

   IfAddress build() const;

   IfAddress buildFromObject(struct rtnl_addr* addr);

   IfAddressBuilder& loadFromObject(struct rtnl_addr* addr);

   // Required
   IfAddressBuilder& setIfIndex(int ifIndex);

   int getIfIndex() const;

   IfAddressBuilder& setPrefix(const folly::CIDRNetwork& prefix);

   folly::Optional<folly::CIDRNetwork> getPrefix() const;

   IfAddressBuilder& setFamily(uint8_t family);

   // Family will be shadowed if prefix is set
   folly::Optional<uint8_t> getFamily() const;

   IfAddressBuilder& setScope(uint8_t scope);

   folly::Optional<uint8_t> getScope() const;

   IfAddressBuilder& setFlags(uint8_t flags);

   folly::Optional<uint8_t> getFlags() const;

   IfAddressBuilder& setValid(bool isValid);

   bool isValid() const;

   // Reset builder, all the required fields need to be set again
   void reset();

 private:
   folly::Optional<folly::CIDRNetwork> prefix_;
   int ifIndex_{0};
   bool isValid_{false};
   folly::Optional<uint8_t> scope_;
   folly::Optional<uint8_t> flags_;
   folly::Optional<uint8_t> family_;
};

// Wrapper class fo rtnl_addr
class IfAddress final {
 public:
   explicit IfAddress(const IfAddressBuilder& builder);
   ~IfAddress();

   IfAddress(IfAddress&&) noexcept;

   IfAddress& operator=(IfAddress&&) noexcept;

   // Family will be shadowed if prefix is set
   uint8_t getFamily() const;

   uint8_t getPrefixLen() const;

   int getIfIndex() const;

   folly::Optional<folly::CIDRNetwork> getPrefix() const;

   folly::Optional<uint8_t> getScope() const;

   folly::Optional<uint8_t> getFlags() const;

   bool isValid() const;

   /**
    * Will construct rtnl_addr object on the first time call, then will return
    * the same object pointer. It will just return the pointer
    * without increase the ref count. Caller shouldn't do rtnl_route_put
    * without explicit increase of it's ref count
    */
   struct rtnl_addr* fromIfAddress() const;

 private:

   void init();

   folly::Optional<folly::CIDRNetwork> prefix_;
   int ifIndex_{0};
   bool isValid_{false};
   folly::Optional<uint8_t> scope_;
   folly::Optional<uint8_t> flags_;
   folly::Optional<uint8_t> family_;
   struct rtnl_addr* ifAddr_{nullptr};
};

class Neighbor;
class NeighborBuilder final {
 public:
   NeighborBuilder() {}
   ~NeighborBuilder() {}

   // Only support V6 neighbor
   Neighbor buildFromObject(struct rtnl_neigh* obj, bool deleted=false) const;

   /**
    * Build Neighbor object to add/del neighbors
    * Add Neighbor:
    *   IfIndex, destination, state must be set
    * Del Neighbor:
    *   Neighbours are uniquely identified by their interface index and
    *   destination address, you may fill out other attributes but they
    *   will have no influence.
    */
   Neighbor build() const;

   NeighborBuilder& setIfIndex(int ifIndex);

   int getIfIndex() const;

   NeighborBuilder& setDestination(const folly::IPAddress& dest);

   folly::IPAddress getDestination() const;

   NeighborBuilder& setLinkAddress(const folly::MacAddress& linkAddress);

   folly::Optional<folly::MacAddress> getLinkAddress() const;

   bool getIsReachable() const;

   /**
    * NUD_INCOMPLETE
    * NUD_REACHABLE
    * NUD_STALE
    * NUD_DELAY
    * NUD_PROBE
    * NUD_FAILED
    * NUD_NOARP
    * NUD_PERMANENT
    */
   NeighborBuilder& setState(int state, bool deleted=false);

   folly::Optional<int> getState() const;

 private:
   int ifIndex_{0};
   bool isReachable_{false};
   folly::IPAddress destination_;
   folly::Optional<folly::MacAddress> linkAddress_;
   folly::Optional<int> state_;
};

// Wrapper class for rtnl_neigh
class Neighbor final {
 public:
   explicit Neighbor(const NeighborBuilder& builder);
   ~Neighbor();

   Neighbor(Neighbor&&) noexcept;

   Neighbor& operator=(Neighbor&&) noexcept;

   int getIfIndex() const;

   int getFamily() const;

   folly::IPAddress getDestination() const;

   folly::Optional<folly::MacAddress> getLinkAddress() const;

   folly::Optional<int> getState() const;

   struct rtnl_neigh* fromNeighbor() const;

   bool isReachable() const;

 private:

   Neighbor(const Neighbor&);
   Neighbor& operator=(const Neighbor&);

   void init();

   int ifIndex_{0};
   bool isReachable_{false};
   folly::IPAddress destination_;
   folly::Optional<folly::MacAddress> linkAddress_;
   folly::Optional<int> state_;
   struct rtnl_neigh* neigh_{nullptr};
};

class Link;
class LinkBuilder final {
 public:
   LinkBuilder() {}
   ~LinkBuilder() {}

   Link buildFromObject(struct rtnl_link* link) const;

   Link build() const;

   LinkBuilder& setLinkName(const std::string& linkName);

   const std::string& getLinkName() const;

   LinkBuilder& setIfIndex(int ifIndex);

   int getIfIndex() const;

   LinkBuilder& setFlags(uint32_t flags);

   uint32_t getFlags() const;

 private:
   std::string linkName_;
   int ifIndex_{0};
   uint32_t flags_{0};
};

class Link final {
 public:
   explicit Link(const LinkBuilder& builder);

   ~Link();

   Link(Link&&) noexcept;

   Link& operator=(Link&&) noexcept;

   const std::string& getLinkName() const;

   int getIfIndex() const;

   uint32_t getFlags() const;

   bool isUp() const;

   struct rtnl_link* fromLink() const;

 private:

   void init();

   Link(const Link&);
   Link& operator=(const Link&);

   std::string linkName_;
   int ifIndex_{0};
   uint32_t flags_{0};
   struct rtnl_link* link_{nullptr};
};

// Link helper class that records Link attributes on the fly
struct LinkAttribute final {
  bool isUp{false};
  int ifIndex{0};
  std::unordered_set<folly::CIDRNetwork> networks;
};

// Route => prefix and its possible nextHops
using NlUnicastRoutes = std::unordered_map<folly::CIDRNetwork, Route>;

// protocolId=>routes
using NlUnicastRoutesDb = std::unordered_map<uint8_t, NlUnicastRoutes>;

/**
 * Multicast and link routes do not have nextHop IP
 * key=>(destination, ifName)
 * value=> route object
 */
using NlMulticastRoutes = std::unordered_map<
                            std::pair<folly::CIDRNetwork, std::string>, Route>;

// protocolId=>routes
using NlMulticastRoutesDb = std::unordered_map<uint8_t, NlMulticastRoutes>;

using NlLinkRoutes = std::unordered_map<
                            std::pair<folly::CIDRNetwork, std::string>, Route>;

// protocolId=>routes
using NlLinkRoutesDb = std::unordered_map<uint8_t, NlLinkRoutes>;

/**
 * Neighbor Object Helpers
 * Map of neighbors that are reachable
 * link name, destination IP and link Address
 */
using NlNeighbors = std::
    unordered_map<std::pair<std::string, folly::IPAddress>, Neighbor>;

// keyed by link name
using NlLinks = std::unordered_map<std::string, LinkAttribute>;

} // namespace fbnl
} // namespace openr
