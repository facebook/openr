/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/MacAddress.h>

#include <openr/if/gen-cpp2/Types_types.h>
#include <thrift/lib/cpp/util/EnumUtils.h>

extern "C" {
#include <linux/rtnetlink.h>
}

namespace openr::fbnl {

class NlException : public std::runtime_error {
 public:
  explicit NlException(const std::string& msg, int err = 0)
      : std::runtime_error(fmt::format(
            "Error({}) - {}. {} ", err, folly::errnoStr(std::abs(err)), msg)) {}
};

const uint8_t DEFAULT_PROTOCOL_ID = 99;

class NextHop;
class NextHopBuilder final {
 public:
  NextHopBuilder() {}
  ~NextHopBuilder() {}

  NextHop build() const;

  void reset();

  // ifIndex related methods
  NextHopBuilder& setIfIndex(int ifIndex);
  std::optional<int> getIfIndex() const;

  // gateway related methods
  NextHopBuilder& setGateway(const folly::IPAddress& gateway);
  NextHopBuilder& unsetGateway();
  std::optional<folly::IPAddress> getGateway() const;

  // weight related methods
  NextHopBuilder& setWeight(uint8_t weight);
  uint8_t getWeight() const;

  // MPLS label action related methods
  NextHopBuilder& setLabelAction(thrift::MplsActionCode);
  std::optional<thrift::MplsActionCode> getLabelAction() const;

  // SWAP label related methods
  NextHopBuilder& setSwapLabel(uint32_t swapLabel);
  std::optional<uint32_t> getSwapLabel() const;

  // PUSH label related methods
  NextHopBuilder& setPushLabels(const std::vector<int32_t>& pushLabels);
  std::optional<std::vector<int32_t>> getPushLabels() const;

  // ATTN: `family_` will be set based on `gateway_` family.
  // No explicit mutator method provided.
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
 *  RTN_UNICAST       a gateway or direct route(default)
 *  RTN_MULTICAST     a multicast route
 *  RTN_UNSPEC        unknown route
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

  // [REQUIRED] destination related methods
  RouteBuilder& setDestination(const folly::CIDRNetwork& dst);
  const folly::CIDRNetwork& getDestination() const;

  // [REQUIRED] rtm type related methods, default RTN_UNICAST
  RouteBuilder& setType(uint8_t type = RTN_UNICAST);
  uint8_t getType() const;

  // [REQUIRED] rtm/rta routeTable_ related methods, default RT_TABLE_MAIN
  RouteBuilder& setRouteTable(uint32_t routeTable = RT_TABLE_MAIN);
  uint32_t getRouteTable() const;

  // [REQUIRED] rtm protocolId_ related methods, default 99
  RouteBuilder& setProtocolId(uint8_t protocolId = DEFAULT_PROTOCOL_ID);
  uint8_t getProtocolId() const;

  // [REQUIRED] rtm scope_ related methods, default RT_SCOPE_UNIVERSE
  RouteBuilder& setScope(uint8_t scope = RT_SCOPE_UNIVERSE);
  uint8_t getScope() const;

  // [REQUIRED] unicast routes nexthop related methods
  RouteBuilder& addNextHop(const NextHop& nextHop);
  const NextHopSet& getNextHops() const;

  // [REQUIRED] mpls routes label related methods
  RouteBuilder& setMplsLabel(uint32_t mplsLabel);
  std::optional<uint32_t> getMplsLabel() const;

  // route valid flag to differentiate route add/delete
  RouteBuilder& setValid(bool isValid);
  bool isValid() const;

  // rtm flag related methods
  //
  // rtm_flags have the following value and meaning:
  //    RTM_F_NOTIFY     if the route changes, notify the user via rtnetlink
  //    RTM_F_CLONED     route is cloned from another route
  //    RTM_F_EQUALIZE   a multipath equalizer (not yet implemented)
  RouteBuilder& setFlags(uint32_t flags);
  std::optional<uint32_t> getFlags() const;

  // Open/R use this as admin-distance for route programmed
  RouteBuilder& setPriority(uint32_t priority);
  std::optional<uint32_t> getPriority() const;

  // TOS related methods
  RouteBuilder& setTos(uint8_t tos);
  std::optional<uint8_t> getTos() const;

  // MTU related methods
  RouteBuilder& setMtu(uint32_t mtu);
  std::optional<uint32_t> getMtu() const;

  // advMss related methods
  RouteBuilder& setAdvMss(uint32_t tos);
  std::optional<uint32_t> getAdvMss() const;

  // ATTN: `family_` will be set when:
  //    UNICAST: `dst_` is set;
  //    MPLS: `mplsLabel_` is set;
  //
  // No explicit mutator method provided.
  uint8_t getFamily() const;

  RouteBuilder& setMultiPath(bool isMultiPath);
  bool isMultiPath() const;

  void reset();

 private:
  uint8_t type_{RTN_UNICAST};
  uint32_t routeTable_{RT_TABLE_MAIN};
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
  std::optional<uint32_t> mplsLabel_;
  bool isMultiPath_{true};
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

  uint32_t getRouteTable() const;

  uint8_t getProtocolId() const;

  uint8_t getScope() const;

  std::optional<uint32_t> getFlags() const;

  std::optional<uint32_t> getPriority() const;

  std::optional<uint8_t> getTos() const;

  std::optional<uint32_t> getMtu() const;

  std::optional<uint32_t> getAdvMss() const;

  const NextHopSet& getNextHops() const;

  bool isValid() const;

  bool isMultiPath() const;

  void setPriority(uint32_t priority);

  std::string str() const;

  void setNextHops(const NextHopSet& nextHops);

 private:
  uint8_t type_{RTN_UNICAST};
  uint32_t routeTable_{RT_TABLE_MAIN};
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
  std::optional<uint32_t> mplsLabel_;
  bool isMultiPath_{true};
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

  std::optional<uint32_t> getPreferredLft() const;

  IfAddressBuilder& setPreferredLft(uint32_t preferredLft);

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
  std::optional<uint32_t> preferredLft_;
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

  std::optional<uint32_t> getPreferredLft() const;

 private:
  std::optional<folly::CIDRNetwork> prefix_;
  int ifIndex_{0};
  bool isValid_{false};
  std::optional<uint8_t> scope_;
  std::optional<uint8_t> flags_;
  std::optional<uint8_t> family_;
  std::optional<uint32_t> preferredLft_;
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

class GreInfo final {
 public:
  GreInfo(
      const folly::IPAddress& localAddr,
      const folly::IPAddress& remoteAddr,
      uint8_t ttl);
  ~GreInfo();

  // Copy+Move constructor and assignment operator
  GreInfo(GreInfo&&) noexcept;
  GreInfo& operator=(GreInfo&&) noexcept;
  GreInfo(const GreInfo&);
  GreInfo& operator=(const GreInfo&);

  folly::IPAddress getLocalAddr() const;

  folly::IPAddress getRemoteAddr() const;

  uint8_t getTtl() const;

  std::string str() const;

 private:
  // local address
  folly::IPAddress localAddr_;
  // remote address
  folly::IPAddress remoteAddr_;
  // ttl, default value is 0 (inherit for IPv4)
  uint8_t ttl_{0};
};

bool operator==(const GreInfo& lhs, const GreInfo& rhs);

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

  LinkBuilder& setLinkKind(const std::string& linkKind);

  std::optional<std::string> getLinkKind() const;

  LinkBuilder& setGreInfo(const GreInfo& greInfo);

  std::optional<GreInfo> getGreInfo() const;

 private:
  std::string linkName_;
  int ifIndex_{0};
  uint32_t flags_{0};
  std::optional<std::string> linkKind_;
  std::optional<GreInfo> greInfo_;
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

  std::optional<std::string> getLinkKind() const;

  std::optional<GreInfo> getGreInfo() const;

  std::string str() const;

 private:
  std::string linkName_;
  int ifIndex_{0};
  uint32_t flags_{0};
  std::optional<std::string> linkKind_;
  std::optional<GreInfo> greInfo_;
};

bool operator==(const Link& lhs, const Link& rhs);

class Rule final {
 public:
  Rule(
      uint16_t family,
      uint8_t action,
      uint32_t table,
      std::optional<uint32_t> fwmark = std::nullopt,
      std::optional<uint32_t> priority = std::nullopt);

  uint16_t getFamily() const;

  uint8_t getAction() const;

  uint32_t getTable() const;

  std::optional<uint32_t> getFwmark() const;

  std::optional<uint32_t> getPriority() const;

  // FRA_TABLE can override table
  void setTable(uint32_t table);

  // set methods for optional fields
  void setFwmark(uint32_t fwmark);

  void setPriority(uint32_t priority);

  std::string str() const;

 private:
  uint16_t family_{AF_UNSPEC};
  // FR_ACT_*
  uint8_t action_{0};
  // table id
  uint32_t table_{0};

  // optional attributs
  std::optional<uint32_t> fwmark_;
  std::optional<uint32_t> priority_;
};

bool operator==(const Rule& lhs, const Rule& rhs);

} // namespace openr::fbnl
