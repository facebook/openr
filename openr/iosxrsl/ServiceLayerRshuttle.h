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

// nextHop => local interface and nextHop IP.
using NextHops = std::unordered_set<std::pair<std::string, folly::IPAddress>>;

// Route => prefix and its possible nextHops
using UnicastRoutes = std::unordered_map<folly::CIDRNetwork, NextHops>;

class VrfData {
public:
    VrfData() { 
        vrfName.clear(); 
        adminDistance=0; 
        vrfPurgeIntervalSeconds=0;
    }

    VrfData(std::string vrf_name,
            uint8_t admin_distance,
            unsigned int purge_interval);

    VrfData(const VrfData &vrfData);
    std::string vrfName;
    uint8_t adminDistance;
    unsigned int vrfPurgeIntervalSeconds;
};

class RouteDbVrf {
public:
    RouteDbVrf() {
        VrfData vrfData;
    }

    RouteDbVrf(VrfData);
    VrfData vrfData;
    UnicastRoutes unicastRoutesV4_;
    UnicastRoutes unicastRoutesV6_;
};

using RouteDb = std::unordered_map<std::string, RouteDbVrf >;

class IosxrslRshuttle {
public:
    explicit IosxrslRshuttle(
            fbzmq::ZmqEventLoop* zmqEventLoop,
            std::vector<VrfData> vrfSet,
            uint8_t routeProtocolId,
            std::shared_ptr<grpc::Channel> Channel);

    ~IosxrslRshuttle();

  // If adding multipath nextHops for the same prefix at different times,
  // always provide unique nextHops and not cumulative list.

  folly::Future<folly::Unit> addUnicastRoute(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);

  // Delete all next hops associated with prefix
  folly::Future<folly::Unit> deleteUnicastRoute(
      const folly::CIDRNetwork& prefix);


  // Sync route table in IOS-XR RIB  with given route table
  // Basically when there's mismatch between IOS-XR RIB and route table in
  // application, we sync RIB with given data source
  folly::Future<folly::Unit> syncRoutes(UnicastRoutes newRouteDb);

  // get cached unicast routing table
  folly::Future<UnicastRoutes> getUnicastRoutes();

  // Set VRF context for V4 routes
  void setIosxrslRouteVrfV4(std::string vrfName);

  // Set VRF context for V6 routes
  void setIosxrslRouteVrfV6(std::string vrfName);

  // Set VRF context for V4 and V6 routes
  void setIosxrslRouteVrf(std::string vrfName);

  // Method to convert IOS-XR Linux interface names to IOS-XR Interface names
  std::string
  iosxrIfName(std::string ifname);

 private:
  IosxrslRshuttle(const IosxrslRshuttle&) = delete;
  IosxrslRshuttle& operator=(const IosxrslRshuttle&) = delete;

  /**
   * Specific implementation for adding v4 and v6 routes into IOS-XR RIB. This
   * is because so that we can have consistent APIs to user even though IOS-XR SL 
   * has different behaviour for ipv4 and ipv6 route APIs.
   *
   */
  void doAddUnicastRoute(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);
  void doAddUnicastRouteV4(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);
  void doAddUnicastRouteV6(
      const folly::CIDRNetwork& prefix, const NextHops& nextHops);

  void doDeleteUnicastRoute(
      const folly::CIDRNetwork& prefix);
  void deleteUnicastRouteV4(
      const folly::CIDRNetwork& prefix);
  void deleteUnicastRouteV6(
      const folly::CIDRNetwork& prefix);

  UnicastRoutes doGetUnicastRoutes();

  void doSyncRoutes(UnicastRoutes newRouteDb);

  fbzmq::ZmqEventLoop* evl_{nullptr};
  std::thread notifThread_;
  std::shared_ptr<AsyncNotifChannel> asynchandler_;
  std::unique_ptr<IosxrslRoute> iosxrslRoute_;
  std::unique_ptr<IosxrslVrf> iosxrslVrf_;

  const uint8_t routeProtocolId_{0};

  // With Service layer APIs, openR is just another protocol on the
  // IOS-XR system. We maintain a local Route Database for sanity checks 
  // and quicker route queries.
  RouteDb routeDb_;

};

}
