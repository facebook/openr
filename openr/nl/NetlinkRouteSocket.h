/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "NetlinkException.h"

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

namespace {
// too bad 0xFB (251) is taken by gated/ospfase
// We will use this as the proto for our routes
const uint8_t kRouteProtoId = 99;
} // namespace

namespace openr {

// nextHop => local interface and nextHop IP.
using NextHops = std::unordered_set<std::pair<std::string, folly::IPAddress>>;

// Route => prefix and its possible nextHops
using UnicastRoutes = std::unordered_map<folly::CIDRNetwork, NextHops>;

// Multicast and link routes do not have nextHop IP
using MulticastRoutes =
    std::unordered_set<std::pair<folly::CIDRNetwork, std::string>>;

using LinkRoutes =
    std::unordered_set<std::pair<folly::CIDRNetwork, std::string>>;

class NetlinkRoute;

// A simple wrapper over libnl API to add / delete routes
// All routes are added with the provided RT_PROTO ID (routeProtocolId)
// If none is provided, we default to kRouteProtoId
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
      fbzmq::ZmqEventLoop* zmqEventLoop,
      uint8_t routeProtocolId = kRouteProtoId);
  ~NetlinkRouteSocket();

  // If adding multipath nextHops for the same prefix at different times,
  // always provide unique nextHops and not cumulative list.
  // Currently we do not enforce checks from local cache,
  // but kernel will reject the request

  folly::Future<folly::Unit> addUnicastRoute(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);

  // Delete all next hops associated with prefix
  folly::Future<folly::Unit> deleteUnicastRoute(
      const folly::CIDRNetwork& prefix);

  // Throw exceptions if the route already existed
  // This is to prevent duplicate routes in some systems where kernel
  // already added this route for us
  folly::Future<folly::Unit> addMulticastRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);

  folly::Future<folly::Unit> deleteMulticastRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);

  // Sync route table in kernel with given route table
  // Basically when there's mismatch between backend kernel and route table in
  // application, we sync kernel routing table with given data source
  folly::Future<folly::Unit> syncUnicastRoutes(UnicastRoutes newRouteDb);

  folly::Future<folly::Unit> syncLinkRoutes(const LinkRoutes& newRouteDb);

  // get cached unicast routing table
  folly::Future<UnicastRoutes> getUnicastRoutes() const;

  // get kernel unicast routing table
  folly::Future<UnicastRoutes> getKernelUnicastRoutes();

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
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);
  /**
   * For v4 multipapth: All nexthops must be specified while programming a
   * route as kernel wipes out all existing ones.
   *
   * For v6 multipath: Kernel allows addition/deletion of individual routes
   *
   */
  void doAddUpdateUnicastRouteV4(
      const folly::CIDRNetwork& prefix,
      const std::unordered_set<std::pair<std::string, folly::IPAddress>>&
          newNextHops);

  void doAddUpdateUnicastRouteV6(
      const folly::CIDRNetwork& prefix,
      const std::unordered_set<std::pair<std::string, folly::IPAddress>>&
          newNextHops,
      const std::unordered_set<std::pair<std::string, folly::IPAddress>>&
          oldNextHops);

  void doAddMulticastRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);

  /**
   * Delete API has same behavior for v4 and v6: given prefix delete all routes
   */
  void doDeleteUnicastRoute(const folly::CIDRNetwork& prefix);

  void doDeleteMulticastRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);

  /**
   * Update our route cache. Only routes tagged with our protocolID are read.
   * External users can trigger this on demand to sync with routes from kernel.
   */
  void doUpdateRouteCache();

  UnicastRoutes doGetUnicastRoutes() const;

  void doSyncUnicastRoutes(UnicastRoutes newRouteDb);

  void doSyncLinkRoutes(const LinkRoutes& newRouteDb);

  std::unique_ptr<NetlinkRoute> buildUnicastRoute(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);

  std::unique_ptr<NetlinkRoute> buildMulticastRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);

  std::unique_ptr<NetlinkRoute> buildLinkRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);

  std::unique_ptr<NetlinkRoute> buildMulticastOrLinkRouteHelper(
      const folly::CIDRNetwork& prefix,
      const std::string& ifName,
      uint8_t scope);

  fbzmq::ZmqEventLoop* evl_{nullptr};
  const uint8_t routeProtocolId_{0};
  struct nl_sock* socket_{nullptr};
  struct nl_cache* cacheV4_{nullptr};
  struct nl_cache* cacheV6_{nullptr};
  struct nl_cache* linkCache_{nullptr};

  // Keep a local copy of unicast route db to reflect current forwarding state
  // This may out of sync with kernel, need to do explicit sync up
  UnicastRoutes unicastRouteDb_{};
  LinkRoutes linkRouteDb_{};

  // Local cache. We do not use this to enforce any checks
  // for incoming requests. Merely an optimization for getUnicastRoutes()
  UnicastRoutes unicastRoutes_{};

  // Check against redundant multicast routes
  MulticastRoutes mcastRoutes_{};

  LinkRoutes linkRoutes_{};
};

} // namespace openr
