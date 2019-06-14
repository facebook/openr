/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include <folly/Optional.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

extern "C" {
#include <net/if.h>
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include <netlink/route/route.h>
#include <netlink/socket.h>
}

namespace openr {
namespace fbnl {

class NlException : public std::runtime_error {
 public:
  explicit NlException(const std::string& msg)
      : std::runtime_error(folly::sformat("NlException: {} ", msg)) {}
};

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

  NextHopBuilder& setLabelAction(thrift::MplsActionCode);

  NextHopBuilder& setSwapLabel(uint32_t swapLabel);

  NextHopBuilder& setPushLabels(const std::vector<int32_t>& pushLabels);

  folly::Optional<int> getIfIndex() const;

  folly::Optional<folly::IPAddress> getGateway() const;

  folly::Optional<uint8_t> getWeight() const;

  folly::Optional<thrift::MplsActionCode> getLabelAction() const;

  folly::Optional<uint32_t> getSwapLabel() const;

  folly::Optional<std::vector<int32_t>> getPushLabels() const;

  uint8_t getFamily() const;

 private:
  folly::Optional<int> ifIndex_;
  folly::Optional<folly::IPAddress> gateway_;
  folly::Optional<uint8_t> weight_;
  folly::Optional<thrift::MplsActionCode> labelAction_;
  folly::Optional<uint32_t> swapLabel_;
  folly::Optional<std::vector<int32_t>> pushLabels_;
  folly::Optional<uint8_t> family_;
};

// Wrapper class for rtnl_nexthop
// NOTE: No special copy+move constructor and assignment operators for this one
class NextHop final {
 public:
  explicit NextHop(const NextHopBuilder& builder);

  folly::Optional<int> getIfIndex() const;

  folly::Optional<folly::IPAddress> getGateway() const;

  folly::Optional<uint8_t> getWeight() const;

  std::string str() const;

  folly::Optional<thrift::MplsActionCode> getLabelAction() const;

  folly::Optional<uint32_t> getSwapLabel() const;

  folly::Optional<std::vector<int32_t>> getPushLabels() const;

  uint8_t getFamily() const;
  /**
   * This method Will construct rtnl_nexthop object and return it. Owner is
   * responsible for freeing it up. Use this method carefully.
   *
   * NOTE: This method is different from `getRtnl<>Ref` which all other types
   * provides. Reason: Usually rtnl_nexthop object will be added to rtnl_route
   * object which will manage the next-hop object.
   */
  struct rtnl_nexthop* getRtnlNexthopObj() const;

 private:
  // Nexthop build helpers
  struct rtnl_nexthop* buildNextHopInternal(
      int ifIdx, const folly::IPAddress& gateway) const;

  struct rtnl_nexthop* buildNextHopInternal(
      const folly::IPAddress& gateway) const;

  struct rtnl_nexthop* buildNextHopInternal(int ifIdx) const;

 private:
  folly::Optional<int> ifIndex_;
  folly::Optional<folly::IPAddress> gateway_;
  folly::Optional<uint8_t> weight_;
  folly::Optional<thrift::MplsActionCode> labelAction_;
  folly::Optional<uint32_t> swapLabel_;
  folly::Optional<std::vector<int32_t>> pushLabels_;
  folly::Optional<uint8_t> family_;
};

bool operator==(const NextHop& lhs, const NextHop& rhs);

struct NextHopHash {
  size_t operator()(const openr::fbnl::NextHop& nh) const;
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
   * @throw fbnl::NlException on failed
   */
  Route build() const;

  Route buildFromObject(struct rtnl_route* obj);

  RouteBuilder& loadFromObject(struct rtnl_route* obj);

  /**
   * Build multicast route
   * @required parameter:
   * ProtocolId, Destination, Iface Name, Iface Index
   * @throw fbnl::NlException on failed
   */
  Route buildMulticastRoute() const;

  /**
   * Build link route
   * @required parameter:
   * ProtocolId, Destination, Iface Name, Iface Index
   * @throw fbnl::NlException on failed
   */
  Route buildLinkRoute() const;

  // Required
  RouteBuilder& setDestination(const folly::CIDRNetwork& dst);

  const folly::CIDRNetwork& getDestination() const;

  RouteBuilder& setMplsLabel(uint32_t mplsLabel);

  folly::Optional<uint32_t> getMplsLabel() const;
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

  RouteBuilder& setMtu(uint32_t mtu);

  folly::Optional<uint32_t> getMtu() const;

  RouteBuilder& setAdvMss(uint32_t tos);

  folly::Optional<uint32_t> getAdvMss() const;

  RouteBuilder& addNextHop(const NextHop& nextHop);

  RouteBuilder& setRouteIfName(const std::string& ifName);

  folly::Optional<std::string> getRouteIfName() const;

  // Multicast/Link route only need ifIndex in nexthop
  RouteBuilder& setRouteIfIndex(int ifIndex);

  folly::Optional<int> getRouteIfIndex() const;

  const NextHopSet& getNextHops() const;

  uint8_t getFamily() const;

  void reset();

 private:
  uint8_t type_{RTN_UNICAST};
  uint8_t routeTable_{RT_TABLE_MAIN};
  uint8_t protocolId_{DEFAULT_PROTOCOL_ID};
  uint8_t scope_{RT_SCOPE_UNIVERSE};
  uint8_t family_{AF_UNSPEC};
  bool isValid_{false};
  folly::Optional<uint32_t> flags_;
  folly::Optional<uint32_t> priority_;
  folly::Optional<uint8_t> tos_;
  folly::Optional<uint32_t> mtu_;
  folly::Optional<uint32_t> advMss_;
  NextHopSet nextHops_;
  folly::CIDRNetwork dst_;
  folly::Optional<int> routeIfIndex_; // for multicast or link route
  folly::Optional<std::string> routeIfName_; // for multicast or linkroute
  folly::Optional<uint32_t> mplsLabel_;
};

// Wrapper class for rtnl_route
class Route final {
 public:
  explicit Route(const RouteBuilder& builder);
  ~Route();

  // Copy+Move constructor and assignment operator
  Route(Route&&) noexcept;
  Route& operator=(Route&&) noexcept;
  Route(const Route&);
  Route& operator=(const Route&);

  uint8_t getFamily() const;

  const folly::CIDRNetwork& getDestination() const;

  folly::Optional<uint32_t> getMplsLabel() const;

  uint8_t getType() const;

  uint8_t getRouteTable() const;

  uint8_t getProtocolId() const;

  uint8_t getScope() const;

  folly::Optional<uint32_t> getFlags() const;

  folly::Optional<uint32_t> getPriority() const;

  folly::Optional<uint8_t> getTos() const;

  folly::Optional<uint32_t> getMtu() const;

  folly::Optional<uint32_t> getAdvMss() const;

  const NextHopSet& getNextHops() const;

  bool isValid() const;

  folly::Optional<std::string> getRouteIfName() const;

  void setPriority(uint32_t priority);

  std::string str() const;

  /**
   * This method Will construct rtnl_route object on the first time call,
   * then will return the same object pointer. It will just return the pointer
   * without increase the ref count. Caller shouldn't do rtnl_route_put
   * without explicit increase of it's ref count
   * getRtnlRouteRef => Returns full rtnl_route object including nexthops
   * getRtnlRouteKeyRef => Returns partial rtnl_route object just containining
   *                       key (prefix) and no nexthhops
   */
  struct rtnl_route* getRtnlRouteRef();
  struct rtnl_route* getRtnlRouteKeyRef();

 private:
  struct nl_addr* buildAddrObject(const folly::CIDRNetwork& addr);
  struct rtnl_route* createRtnlRouteKey();

  uint8_t type_{RTN_UNICAST};
  uint8_t routeTable_{RT_TABLE_MAIN};
  uint8_t protocolId_{DEFAULT_PROTOCOL_ID};
  uint8_t scope_{RT_SCOPE_UNIVERSE};
  uint8_t family_{AF_UNSPEC};
  bool isValid_{false};
  folly::Optional<uint32_t> flags_;
  folly::Optional<uint32_t> priority_;
  folly::Optional<uint8_t> tos_;
  folly::Optional<uint32_t> mtu_;
  folly::Optional<uint32_t> advMss_;
  NextHopSet nextHops_;
  folly::CIDRNetwork dst_;
  folly::Optional<std::string> routeIfName_;
  struct rtnl_route* route_{nullptr};
  struct rtnl_route* routeKey_{nullptr};
  folly::Optional<uint32_t> mplsLabel_;
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

  // Copy+Move constructor and assignment operator
  IfAddress(IfAddress&&) noexcept;
  IfAddress& operator=(IfAddress&&) noexcept;
  IfAddress(const IfAddress&);
  IfAddress& operator=(const IfAddress&);

  // Family will be shadowed if prefix is set
  uint8_t getFamily() const;

  uint8_t getPrefixLen() const;

  int getIfIndex() const;

  folly::Optional<folly::CIDRNetwork> getPrefix() const;

  folly::Optional<uint8_t> getScope() const;

  folly::Optional<uint8_t> getFlags() const;

  bool isValid() const;

  std::string str() const;

  /**
   * Will construct rtnl_addr object on the first time call, then will return
   * the same object pointer. It will just return the pointer
   * without increase the ref count. Caller shouldn't do rtnl_addr_put
   * without explicit increase of it's ref count
   */
  struct rtnl_addr* getRtnlAddrRef();

 private:
  folly::Optional<folly::CIDRNetwork> prefix_;
  int ifIndex_{0};
  bool isValid_{false};
  folly::Optional<uint8_t> scope_;
  folly::Optional<uint8_t> flags_;
  folly::Optional<uint8_t> family_;
  struct rtnl_addr* ifAddr_{nullptr};
};

bool operator==(const IfAddress& lhs, const IfAddress& rhs);

class Neighbor;
class NeighborBuilder final {
 public:
  NeighborBuilder() {}
  ~NeighborBuilder() {}

  // Only support V6 neighbor
  Neighbor buildFromObject(struct rtnl_neigh* obj, bool deleted = false) const;

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
  NeighborBuilder& setState(int state, bool deleted = false);

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

  // Copy+Move constructor and assignment operator
  Neighbor(Neighbor&&) noexcept;
  Neighbor& operator=(Neighbor&&) noexcept;
  Neighbor(const Neighbor&);
  Neighbor& operator=(const Neighbor&);

  int getIfIndex() const;

  bool isReachable() const;

  int getFamily() const;

  folly::IPAddress getDestination() const;

  folly::Optional<folly::MacAddress> getLinkAddress() const;

  folly::Optional<int> getState() const;

  std::string str() const;

  struct rtnl_neigh* getRtnlNeighRef();

 private:
  int ifIndex_{0};
  bool isReachable_{false};
  folly::IPAddress destination_;
  folly::Optional<folly::MacAddress> linkAddress_;
  folly::Optional<int> state_;
  struct rtnl_neigh* neigh_{nullptr};
};

bool operator==(const Neighbor& lhs, const Neighbor& rhs);

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

  // Copy+Move constructor and assignment operator
  Link(Link&&) noexcept;
  Link& operator=(Link&&) noexcept;
  Link(const Link&);
  Link& operator=(const Link&);

  const std::string& getLinkName() const;

  int getIfIndex() const;

  uint32_t getFlags() const;

  bool isUp() const;

  bool isLoopback() const;

  std::string str() const;

  struct rtnl_link* getRtnlLinkRef();

 private:
  std::string linkName_;
  int ifIndex_{0};
  uint32_t flags_{0};
  struct rtnl_link* link_{nullptr};
};

bool operator==(const Link& lhs, const Link& rhs);

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

// MPLS => label and its possible nextHops
using NlMplsRoutes = std::unordered_map<int32_t, Route>;

// protocolId=>label routes
using NlMplsRoutesDb = std::unordered_map<uint8_t, NlMplsRoutes>;

/**
 * Multicast and link routes do not have nextHop IP
 * key => (destination, ifName)
 * value => route object
 */
using NlMulticastRoutes =
    std::unordered_map<std::pair<folly::CIDRNetwork, std::string>, Route>;

// protocolId => routes
using NlMulticastRoutesDb = std::unordered_map<uint8_t, NlMulticastRoutes>;

using NlLinkRoutes =
    std::unordered_map<std::pair<folly::CIDRNetwork, std::string>, Route>;

// protocolId => routes
using NlLinkRoutesDb = std::unordered_map<uint8_t, NlLinkRoutes>;

/**
 * Neighbor Object Helpers
 * Map of neighbors that are reachable
 * link name, destination IP and link Address
 */
using NlNeighbors =
    std::unordered_map<std::pair<std::string, folly::IPAddress>, Neighbor>;

// keyed by link name
using NlLinks = std::unordered_map<std::string, LinkAttribute>;

} // namespace fbnl
} // namespace openr
