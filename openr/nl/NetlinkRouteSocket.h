/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "NetlinkException.h"
#include "NetlinkTypes.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/futures/Future.h>
#include <openr/common/AddressUtil.h>
#include <openr/common/Util.h>

extern "C" {
#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/netlink.h>
#include <netlink/route/link.h>
#include <netlink/route/route.h>
#include <netlink/socket.h>
}

namespace openr {

// nextHop => local interface and nextHop IP.
using NextHops = std::unordered_set<std::pair<std::string, folly::IPAddress>>;

// Route => prefix and its possible nextHops
using UnicastRoutes = std::unordered_map<folly::CIDRNetwork, NextHops>;

// protocolId=>routes
using UnicastRoutesDb = std::unordered_map<uint8_t, UnicastRoutes>;

// Multicast and link routes do not have nextHop IP
using MulticastRoutes =
    std::unordered_set<std::pair<folly::CIDRNetwork, std::string>>;

// protocolId=>routes
using MulticastRoutesDb = std::unordered_map<uint8_t, MulticastRoutes>;

using LinkRoutes =
    std::unordered_set<std::pair<folly::CIDRNetwork, std::string>>;

// protocolId=>routes
using LinkRoutesDb = std::unordered_map<uint8_t, LinkRoutes>;

struct GetAddrsFuncCtx {
  GetAddrsFuncCtx(int addrIfIndex, int addrFamily, int addrScope)
    : ifIndex(addrIfIndex),
      family(addrFamily),
      scope(addrScope) {}
  int ifIndex{0};
  int family{AF_UNSPEC};
  int scope{RT_SCOPE_NOWHERE};
  std::vector<fbnl::IfAddress> addrs;
};

class NetlinkRoute;

// A simple wrapper over libnl API to add / delete routes
// Users can on the fly request to manipulate routes with any protocol ids
// We are stateless to the effect that we do not check against duplicate
// route additions and non existing route removals. It is upto the user
// to enforce such checks. Any such error will cause libnl to error, and
// we raise exception to that
//
// All error handling is via exceptions.
//
// NOTE: Not a thread safe object. If caller has multiple threads calling
// into this object's API, they must handle concurrency (example: locking)

class NetlinkRouteSocket final {
 public:
  explicit NetlinkRouteSocket(
      fbzmq::ZmqEventLoop* zmqEventLoop);
  ~NetlinkRouteSocket();

  // If adding multipath nextHops for the same prefix at different times,
  // always provide unique nextHops and not cumulative list.
  // Currently we do not enforce checks from local cache,
  // but kernel will reject the request

  folly::Future<folly::Unit> addUnicastRoute(
      uint8_t protocolId,
      folly::CIDRNetwork prefix, NextHops nextHops);

  // Delete all next hops associated with prefix
  folly::Future<folly::Unit> deleteUnicastRoute(
      uint8_t protocolId,
      folly::CIDRNetwork prefix);

  // Throw exceptions if the route already existed
  // This is to prevent duplicate routes in some systems where kernel
  // already added this route for us
  folly::Future<folly::Unit> addMulticastRoute(
      uint8_t protocolId,
      folly::CIDRNetwork prefix, std::string ifName);

  folly::Future<folly::Unit> deleteMulticastRoute(
      uint8_t protocolId,
      folly::CIDRNetwork prefix, std::string ifName);

  // Sync route table in kernel with given route table
  // Basically when there's mismatch between backend kernel and route table in
  // application, we sync kernel routing table with given data source
  folly::Future<folly::Unit> syncUnicastRoutes(
      uint8_t protocolId,
      UnicastRoutes newRouteDb);

  folly::Future<folly::Unit> syncLinkRoutes(
      uint8_t protocolId,
      LinkRoutes newRouteDb);

  // get cached unicast routing by protocol ID
  folly::Future<UnicastRoutes>
  getCachedUnicastRoutes(uint8_t protocolId) const;

  // get cached multicast routing by protocol ID
  folly::Future<MulticastRoutes>
  getCachedMulticastRoutes(uint8_t protocolId) const;

  // get cached link route by protocol ID
  folly::Future<LinkRoutes>
  getCachedLinkRoutes(uint8_t protocolId) const;

  // get number of all cached routes
  folly::Future<int64_t> getRouteCount() const;

  // add Interface address e.g. ip addr add 192.168.1.1/24 dev em1
  folly::Future<folly::Unit> addIfAddress(fbnl::IfAddress ifAddr);

  /**
   * Delete Interface address e.g.
   * -- ip addr del 192.168.1.1/24 dev em1
   *
   * Prefix, ifIndex are mandatory, the specific address and
   * interface tuple will be deleted
   */
  folly::Future<folly::Unit> delIfAddress(fbnl::IfAddress ifAddr);

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
  folly::Future<folly::Unit> syncIfAddress(
    int ifIndex,
    std::vector<fbnl::IfAddress> addrs,
    int family = AF_UNSPEC, int scope = RT_SCOPE_NOWHERE);

  // get interface index from name
  // 0 means no such interface
  folly::Future<int> getIfIndex(const std::string& ifName);

  // get interface name form index
  folly::Future<std::string> getIfName(int ifIndex);

  /**
   * Get iface address list according
   */
  folly::Future<std::vector<fbnl::IfAddress>> getIfAddrs(
    int ifIndex, int family = AF_UNSPEC, int scope = RT_SCOPE_NOWHERE);

 private:
  NetlinkRouteSocket(const NetlinkRouteSocket&) = delete;
  NetlinkRouteSocket& operator=(const NetlinkRouteSocket&) = delete;

  /**
   * Specific implementation for adding v4 and v6 routes into the kernel. This
   * is because so that we can have consistent APIs to user even though kernel
   * has different behaviour for ipv4 and ipv6 route UAPIs.
   *
   */
  void doAddUpdateUnicastRoute(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix,
      const NextHops& nextHops);
  /**
   * For v4 multipapth: All nexthops must be specified while programming a
   * route as kernel wipes out all existing ones.
   *
   * For v6 multipath: Kernel allows addition/deletion of individual routes
   *
   */
  void doAddUpdateUnicastRouteV4(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix,
      const NextHops& newNextHops);

  void doAddUpdateUnicastRouteV6(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix,
      const NextHops& newNextHops,
      const NextHops& oldNextHops);

  void doAddMulticastRoute(
      uint8_t routeProtocolId,
      const folly::CIDRNetwork& prefix,
      const std::string& ifName);

  /**
   * Delete API has same behavior for v4 and v6: given prefix delete all routes
   */
  void doDeleteUnicastRoute(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix);

  void doDeleteMulticastRoute(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix,
      const std::string& ifName);

  /**
   * Update our route cache for all protocols ids
   */
  void doUpdateRouteCache();

  void doSyncUnicastRoutes(
      uint8_t protocolId,
      const UnicastRoutes& newRouteDb);

  void doSyncLinkRoutes(
      uint8_t protocolId,
      const LinkRoutes& newRouteDb);

  std::unique_ptr<NetlinkRoute> buildUnicastRoute(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix,
      const NextHops& nextHops);

  std::unique_ptr<NetlinkRoute> buildMulticastRoute(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix,
      const std::string& ifName);

  std::unique_ptr<NetlinkRoute> buildLinkRoute(
      uint8_t protocolId,
      const folly::CIDRNetwork& prefix,
      const std::string& ifName);

  std::unique_ptr<NetlinkRoute> buildMulticastOrLinkRouteHelper(
      const folly::CIDRNetwork& prefix,
      const std::string& ifName,
      uint8_t protocolId,
      uint8_t scope);

  void doAddIfAddress(struct rtnl_addr* addr);

  void doDeleteAddr(struct rtnl_addr* addr);

  void doSyncIfAddress(
    int ifIdnex, std::vector<fbnl::IfAddress> addrs, int family, int scope);

  void doGetIfAddrs(
    int ifIndex, int family, int scope,
    std::vector<fbnl::IfAddress>& addrs);

  /**
   * This function will update link cache internally but will ensure that it is
   * updated only once in a second. First call will trigger update and all
   * subsequent calls within a second will be ignored.
   */
  void updateLinkCacheThrottled();

  fbzmq::ZmqEventLoop* evl_{nullptr};
  const uint8_t routeProtocolId_{0};
  struct nl_sock* socket_{nullptr};
  struct nl_cache* cacheV4_{nullptr};
  struct nl_cache* cacheV6_{nullptr};
  struct nl_cache* linkCache_{nullptr};

  // Timer to refresh linkCache_ to pick up new interface information
  std::chrono::steady_clock::time_point linkCacheUpdateTs_{
    std::chrono::steady_clock::now() - Constants::kNetlinkSyncThrottleInterval};

  // Local cache. We do not use this to enforce any checks
  // for incoming requests. Merely an optimization for getUnicastRoutes()
  UnicastRoutesDb unicastRoutesCache_;

  // Check against redundant multicast routes
  MulticastRoutesDb mcastRoutesCache_;

  LinkRoutesDb linkRoutesCache_;
};

} // namespace openr
