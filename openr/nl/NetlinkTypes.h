/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <folly/Optional.h>
#include <folly/IPAddress.h>

extern "C" {
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/route.h>
#include <netlink/socket.h>
}

namespace openr {
namespace fbnl {

const uint8_t DEFAULT_PROTOCOL_ID = 99;

class NextHop;
class NextHopBuilder final {
 public:
   NextHopBuilder() {}
   ~NextHopBuilder() {}

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

  Route build() const;

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

  uint8_t getScope() const;

  RouteBuilder& setFlags(uint32_t flags);

  folly::Optional<uint32_t> getFlags() const;

  RouteBuilder& setPriority(uint32_t priority);

  folly::Optional<uint32_t> getPriority() const;

  RouteBuilder& setTos(uint8_t tos);

  folly::Optional<uint8_t> getTos() const;

  RouteBuilder& addNextHop(const NextHop& nextHop);

  const std::vector<NextHop>&
  getNextHops() const;

 private:
  uint8_t type_{RTN_UNICAST};
  uint8_t routeTable_{RT_TABLE_MAIN};
  uint8_t protocolId_{DEFAULT_PROTOCOL_ID};
  uint8_t scope_{RT_SCOPE_UNIVERSE};
  folly::Optional<uint32_t> flags_;
  folly::Optional<uint32_t> priority_;
  folly::Optional<uint8_t> tos_;
  std::vector<NextHop> nextHops_;
  folly::CIDRNetwork dst_;
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

   const std::vector<NextHop>&
   getNextHops() const;

   /**
    * This method Will construct rtnl_route object on the first time call,
    * then will return the same object pointer. It will just return the pointer
    * without increase the ref count. Caller shouldn't do rtnl_route_put
    * without explicit increase of it's ref count
    */
   struct rtnl_route* fromNetlinkRoute() const;

 private:
   Route(const Route&);
   Route& operator=(const Route&);

   void init();
   struct nl_addr* buildAddrObject(const folly::CIDRNetwork& addr);

   uint8_t type_;
   uint8_t routeTable_;
   uint8_t protocolId_;
   uint8_t scope_;
   folly::Optional<uint32_t> flags_;
   folly::Optional<uint32_t> priority_;
   folly::Optional<uint8_t> tos_;
   std::vector<NextHop> nextHops_;
   folly::CIDRNetwork dst_;
   struct rtnl_route* route_{nullptr};
};

class IfAddress;
class IfAddressBuilder final {
 public:
   IfAddressBuilder() {}
   ~IfAddressBuilder() {}

   IfAddress build();

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

   // Reset builder, all the required fields need to be set again
   void reset();

 private:
   folly::Optional<folly::CIDRNetwork> prefix_;
   int ifIndex_;
   folly::Optional<uint8_t> scope_;
   folly::Optional<uint8_t> flags_;
   folly::Optional<uint8_t> family_;
};

// Wrapper class fo rtnl_addr
class IfAddress final {
 public:
   explicit IfAddress(IfAddressBuilder& builder);
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
   folly::Optional<uint8_t> scope_;
   folly::Optional<uint8_t> flags_;
   folly::Optional<uint8_t> family_;
   struct rtnl_addr* ifAddr_{nullptr};
};

} // namespace fbnl
} // namespace openr
