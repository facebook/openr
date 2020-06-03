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
#include <linux/rtnetlink.h>
#include <net/if.h>
}

namespace openr::fbnl {

class NlException : public std::runtime_error {
 public:
  explicit NlException(const std::string& msg, int err = 0)
      : std::runtime_error(folly::sformat(
            "Error({}) - {}. {} ", err, folly::errnoStr(err), msg)) {}
};

const uint8_t DEFAULT_PROTOCOL_ID = 99;

enum NetlinkEventType {
  LINK_EVENT = 0,
  NEIGH_EVENT,
  ADDR_EVENT,
  MAX_EVENT_TYPE // sentinel
};

class NextHop;
class NextHopBuilder final {
 public:
  NextHopBuilder() {}
  ~NextHopBuilder() {}

  NextHop build() const;

  void reset();

  NextHopBuilder& setIfIndex(int ifIndex);

  NextHopBuilder& setGateway(const folly::IPAddress& gateway);
  NextHopBuilder&
  unsetGateway() {
    gateway_.reset();
    return *this;
  }

  NextHopBuilder& setWeight(uint8_t weight);

  NextHopBuilder& setLabelAction(thrift::MplsActionCode);

  NextHopBuilder& setSwapLabel(uint32_t swapLabel);

  NextHopBuilder& setPushLabels(const std::vector<int32_t>& pushLabels);

  std::optional<int> getIfIndex() const;

  std::optional<folly::IPAddress> getGateway() const;

  uint8_t getWeight() const;

  std::optional<thrift::MplsActionCode> getLabelAction() const;

  std::optional<uint32_t> getSwapLabel() const;

  std::optional<std::vector<int32_t>> getPushLabels() const;

  uint8_t getFamily() const;

 private:
  std::optional<int> ifIndex_;
  std::optional<folly::IPAddress> gateway_;
  uint8_t weight_{0}; // default weight is 0
  std::optional<thrift::MplsActionCode> labelAction_;
  std::optional<uint32_t> swapLabel_;
  std::optional<std::vector<int32_t>> pushLabels_;
  std::optional<uint8_t> family_;
};

// NOTE: No special copy+move constructor and assignment operators for this one
class NextHop final {
 public:
  explicit NextHop(const NextHopBuilder& builder);

  std::optional<int> getIfIndex() const;

  std::optional<folly::IPAddress> getGateway() const;

  uint8_t getWeight() const;

  std::string str() const;

  std::optional<thrift::MplsActionCode> getLabelAction() const;

  std::optional<uint32_t> getSwapLabel() const;

  std::optional<std::vector<int32_t>> getPushLabels() const;

  void setPushLabels(std::vector<int32_t>);

  uint8_t getFamily() const;

 private:
  std::optional<int> ifIndex_;
  std::optional<folly::IPAddress> gateway_;
  uint8_t weight_{0}; // default weight is 0
  std::optional<thrift::MplsActionCode> labelAction_;
  std::optional<uint32_t> swapLabel_;
  std::optional<std::vector<int32_t>> pushLabels_;
  std::optional<uint8_t> family_;
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

  std::optional<uint32_t> getMplsLabel() const;
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

  std::optional<uint32_t> getFlags() const;

  RouteBuilder& setPriority(uint32_t priority);

  std::optional<uint32_t> getPriority() const;

  RouteBuilder& setTos(uint8_t tos);

  std::optional<uint8_t> getTos() const;

  RouteBuilder& setMtu(uint32_t mtu);

  std::optional<uint32_t> getMtu() const;

  RouteBuilder& setAdvMss(uint32_t tos);

  std::optional<uint32_t> getAdvMss() const;

  RouteBuilder& addNextHop(const NextHop& nextHop);

  RouteBuilder& setRouteIfName(const std::string& ifName);

  std::optional<std::string> getRouteIfName() const;

  // Multicast/Link route only need ifIndex in nexthop
  RouteBuilder& setRouteIfIndex(int ifIndex);

  std::optional<int> getRouteIfIndex() const;

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
  std::optional<uint32_t> flags_;
  std::optional<uint32_t> priority_;
  std::optional<uint8_t> tos_;
  std::optional<uint32_t> mtu_;
  std::optional<uint32_t> advMss_;
  NextHopSet nextHops_;
  folly::CIDRNetwork dst_;
  std::optional<int> routeIfIndex_; // for multicast or link route
  std::optional<std::string> routeIfName_; // for multicast or linkroute
  std::optional<uint32_t> mplsLabel_;
};

class Route final {
 public:
  Route() {} // Empty constructor
  explicit Route(const RouteBuilder& builder);
  ~Route();

  // Copy+Move constructor and assignment operator
  Route(Route&&) noexcept;
  Route& operator=(Route&&) noexcept;
  Route(const Route&);
  Route& operator=(const Route&);

  uint8_t getFamily() const;

  const folly::CIDRNetwork& getDestination() const;

  std::optional<uint32_t> getMplsLabel() const;

  uint8_t getType() const;

  uint8_t getRouteTable() const;

  uint8_t getProtocolId() const;

  uint8_t getScope() const;

  std::optional<uint32_t> getFlags() const;

  std::optional<uint32_t> getPriority() const;

  std::optional<uint8_t> getTos() const;

  std::optional<uint32_t> getMtu() const;

  std::optional<uint32_t> getAdvMss() const;

  const NextHopSet& getNextHops() const;

  bool isValid() const;

  std::optional<std::string> getRouteIfName() const;

  void setPriority(uint32_t priority);

  std::string str() const;

  void setNextHops(const NextHopSet& nextHops);

 private:
  uint8_t type_{RTN_UNICAST};
  uint8_t routeTable_{RT_TABLE_MAIN};
  uint8_t protocolId_{DEFAULT_PROTOCOL_ID};
  uint8_t scope_{RT_SCOPE_UNIVERSE};
  uint8_t family_{AF_UNSPEC};
  bool isValid_{false};
  std::optional<uint32_t> flags_;
  std::optional<uint32_t> priority_;
  std::optional<uint8_t> tos_;
  std::optional<uint32_t> mtu_;
  std::optional<uint32_t> advMss_;
  NextHopSet nextHops_;
  folly::CIDRNetwork dst_;
  std::optional<std::string> routeIfName_;
  std::optional<uint32_t> mplsLabel_;
};

bool operator==(const Route& lhs, const Route& rhs);

class IfAddress;
class IfAddressBuilder final {
 public:
  IfAddressBuilder() {}
  ~IfAddressBuilder() {}

  IfAddress build() const;

  // Required
  IfAddressBuilder& setIfIndex(int ifIndex);

  int getIfIndex() const;

  IfAddressBuilder& setPrefix(const folly::CIDRNetwork& prefix);

  std::optional<folly::CIDRNetwork> getPrefix() const;

  IfAddressBuilder& setFamily(uint8_t family);

  // Family will be shadowed if prefix is set
  std::optional<uint8_t> getFamily() const;

  IfAddressBuilder& setScope(uint8_t scope);

  std::optional<uint8_t> getScope() const;

  IfAddressBuilder& setFlags(uint8_t flags);

  std::optional<uint8_t> getFlags() const;

  IfAddressBuilder& setValid(bool isValid);

  bool isValid() const;

  // Reset builder, all the required fields need to be set again
  void reset();

 private:
  std::optional<folly::CIDRNetwork> prefix_;
  int ifIndex_{0};
  bool isValid_{false};
  std::optional<uint8_t> scope_;
  std::optional<uint8_t> flags_;
  std::optional<uint8_t> family_;
};

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

  std::optional<folly::CIDRNetwork> getPrefix() const;

  std::optional<uint8_t> getScope() const;

  std::optional<uint8_t> getFlags() const;

  bool isValid() const;

  std::string str() const;

 private:
  std::optional<folly::CIDRNetwork> prefix_;
  int ifIndex_{0};
  bool isValid_{false};
  std::optional<uint8_t> scope_;
  std::optional<uint8_t> flags_;
  std::optional<uint8_t> family_;
};

bool operator==(const IfAddress& lhs, const IfAddress& rhs);

class Neighbor;
class NeighborBuilder final {
 public:
  NeighborBuilder() {}
  ~NeighborBuilder() {}

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

  std::optional<folly::MacAddress> getLinkAddress() const;

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

  std::optional<int> getState() const;

 private:
  int ifIndex_{0};
  bool isReachable_{false};
  folly::IPAddress destination_;
  std::optional<folly::MacAddress> linkAddress_;
  std::optional<int> state_;
};

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

  std::optional<folly::MacAddress> getLinkAddress() const;

  std::optional<int> getState() const;

  std::string str() const;

 private:
  int ifIndex_{0};
  bool isReachable_{false};
  folly::IPAddress destination_;
  std::optional<folly::MacAddress> linkAddress_;
  std::optional<int> state_;
};

bool operator==(const Neighbor& lhs, const Neighbor& rhs);
bool isNeighborReachable(int state);

class Link;
class LinkBuilder final {
 public:
  LinkBuilder() {}
  ~LinkBuilder() {}

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
  Link() {}
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

 private:
  std::string linkName_;
  int ifIndex_{0};
  uint32_t flags_{0};
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

} // namespace openr::fbnl
