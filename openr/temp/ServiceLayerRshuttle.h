#pragma once

#include "ServiceLayerRoute.h"

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


namespace openr {

// too bad 0xFB (251) is taken by gated/ospfase
// We will use this as the proto for our routes
const uint8_t kRouteProtoId = 99;
} // namespace


namespace openr {

class VrfData {
    VrfData() { 
        vrfName.clear(); 
        adminDistance=0; 
        vrfPurgeIntervalSeconds=0;
    }

    VrfData(std::string vrf_name,
            uint8_t admin_distance,
            unsigned int purge_interval);

    VrfData(const Vrfdata &vrfData);

    std::string vrfName;
    uint8_t adminDistance;
    unsigned int vrfPurgeIntervalSeconds;
};

class RouteDbVrf {
    RouteDbVrf(VrfData);
    VrfData vrfData;
    UnicastRoutes unicastRoutes_;
};

// nextHop => local interface and nextHop IP.
using NextHops = std::unordered_set<std::pair<std::string, folly::IPAddress>>;

// Route => prefix and its possible nextHops
using UnicastRoutes = std::unordered_map<folly::CIDRNetwork, NextHops>;

using RouteDb = std::unordered_map<std::string, RouteDbVrf >; 

class IosxrslRshuttle {
public:
    explicit IosxrslRshuttle(
            fbzmq::ZmqEventLoop* zmqEventLoop,
            vector<VrfData> vrfSet,
            std::shared_ptr<grpc::Channel> Channel);

    ~IosxrslRshuttle();

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
 // folly::Future<folly::Unit> addMulticastRoute(
 //     const folly::CIDRNetwork& prefix, const std::string& ifName);

 // folly::Future<folly::Unit> deleteMulticastRoute(
  //    const folly::CIDRNetwork& prefix, const std::string& ifName);

  // Sync route table in kernel with given route table
  // Basically when there's mismatch between backend kernel and route table in
  // application, we sync kernel routing table with given data source
  folly::Future<folly::Unit> syncRoutes(UnicastRoutes newRouteDb);

  // get cached unicast routing table
  folly::Future<UnicastRoutes> getUnicastRoutes() const;

  // get kernel unicast routing table
  folly::Future<UnicastRoutes> getIosxrUnicastRoutes();
 private:
  IosxrslRshuttle(const IosxrslRshuttle&) = delete;
  IosxrslRshuttle& operator=(const IosxrslRshuttle&) = delete;

  /**
   * Specific implementation for adding v4 and v6 routes into the kernel. This
   * is because so that we can have consistent APIs to user even though kernel
   * has different behaviour for ipv4 and ipv6 route UAPIs.
   *
   * For v4 multipapth: All nexthops must be specified while programming a
   * route as kernel wipes out all existing ones.
   *
   * For v6 multipath: Kernel allows addition/deletion of individual routes
   *
   */
  void doAddUnicastRoute(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);
  void doAddUnicastRouteV4(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);
  void doAddUnicastRouteV6(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);

  void doAddMulticastRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);

  void doDeleteUnicastRoute(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);
  void deleteUnicastRouteV4(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);
  void deleteUnicastRouteV6(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);

  void doDeleteMulticastRoute(
      const folly::CIDRNetwork& prefix, const std::string& ifName);


  UnicastRoutes doGetUnicastRoutes() const;

  void doSyncRoutes(UnicastRoutes newRouteDb);

  std::unique_ptr<IosxrslRoute> buildUnicastRoute(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);

  //std::unique_ptr<IosxrslRoute> buildMulticastRoute(
   //   const folly::CIDRNetwork& prefix, const std::string& ifName);

  void doUpdateRoute(
      const folly::CIDRNetwork& prefix,
      const std::unordered_set<std::pair<std::string, folly::IPAddress>>&
          newNextHops,
      const std::unordered_set<std::pair<std::string, folly::IPAddress>>&
          oldNextHops);

  fbzmq::ZmqEventLoop* evl_{nullptr};
  std::thread notifThread_;
  std::unique_ptr<AsyncNotifChannel> asynchandler_;
  std::unique_ptr<IosxrslRoute> iosxrslRoute_;
  std::unique_ptr<IosxrslVrf> iosxrslVrf_;

  const uint8_t routeProtocolId_{0};
  //service_layer::SLRoutev4 routecacheV4_;
  //service_layer::SLRoutev6 routecacheV6_;

  //struct nl_sock* socket_{nullptr};
  //struct nl_cache* cacheV4_{nullptr};
  //struct nl_cache* cacheV6_{nullptr};
  //struct nl_cache* linkCache_{nullptr};

  // With Service layer APIs, openR is just another protocol on the
  // IOS-XR system. We maintain a local Route Database for sanity checks 
  // and quicker route queries.
  RouteDb routeDb_;

  // Check against redundant multicast routes
  //MulticastRoutes mcastRoutes_{};

};

}
