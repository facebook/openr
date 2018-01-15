#include "ServiceLayerRshuttle.h"
#include "ServiceLayerException.h"

#include <algorithm>
#include <memory>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/gen/Base.h>
#include <folly/gen/Core.h>



using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace {

const int kIpAddrBufSize = 1024;
//const uint32_t kAqRouteTableId = ;
const uint8_t kAqRouteProtoId = 99;

// iproute2 protocol IDs in the kernel are a shared resource
// Various well known and custom protocols use it
// This is a *Weak* attempt to protect against some already
// known protocols
//const uint8_t kMinRouteProtocolId = 17;
//const uint8_t kMaxRouteProtocolId = 245;

} //anonymous namespace


namespace openr {

VrfData::VrfData(std::string vrf_name,
            uint8_t admin_distance,
            unsigned int purge_interval)
    : vrfName(vrf_name), 
      adminDistance(admin_distance),
      vrfPurgeIntervalSeconds(purge_interval) {}

Vrfdata::VrfData(const VrfData &vrfData)
{
    vrfName = vrfData.vrfName;
    adminDistance = vrfData.adminDistance;
    vrfPurgeIntervalSeconds = 
            vrfData.vrfPurgeIntervalSeconds;
}


RouteDbvrf::RouteDbvrf(VrfData vrf_data)
    : vrfData(vrf_data) {}


IosxrslRshuttle::IosxrslRshuttle(
            fbzmq::ZmqEventLoop* zmqEventLoop,
            vector<VrfData> vrfSet,
            std::shared_ptr<grpc::Channel> Channel)
{
    evl_ = zmqEventLoop;
    CHECK(evl_) << "Invalid ZMQ event loop handle";

    // Let's set up the AsyncNotifChannel to establish
    // connection to IOS-XR Service Layer 
    asynchandler_ = std::make_unique<AsyncNotifChannel>(channel);


    // Acquire the init lock, register vrf(s) only after condition variable is activated
    std::unique_lock<std::mutex> initlock(init_mutex);
    
    // Spawn reader thread that maintains our Notification Channel
    notifThread_ = std::thread(&AsyncNotifChannel::AsyncCompleteRpc, &asynchandler_);

    service_layer::SLInitMsg init_msg;
    init_msg.set_majorver(service_layer::SL_MAJOR_VERSION);
    init_msg.set_minorver(service_layer::SL_MINOR_VERSION);
    init_msg.set_subver(service_layer::SL_SUB_VERSION);


    asynchandler_->SendInitMsg(init_msg);    
    
    // Wait on the mutex lock
    while (!init_success) {
        init_condVar.wait(initlock);
    }

    iosxrslVrf_ = std::make_unique<IosxrslVrf>(channel);
    iosxrslRoute_ = std::make_unique<IosxrslRoute>(channel);

    evl_->runInEventLoop([this]() mutable {
        for (auto vrf_data : vrfSet) {
            // Create a new SLVrfRegMsg batch
            iosxrslVrf_->vrfRegMsgAdd(
                             vrf_data.vrfName, 
                             vrf_data.adminDistance,
                             vrf_data.vrfPurgeIntervalSeconds);
    
            // Register the SLVrfRegMsg batch for v4 and v6
            iosxrslVrf_->registerVrf(AF_INET);
            iosxrslVrf_->registerVrf(AF_INET6);

            // Set up the local Route DB for each vrf
            routeDb_.emplace(vrf_data.vrfName,RouteDbVrf(vrf_data));
        }
    });
     
}


IosxrslRshuttle::~IosxrslRshuttle()
{
    // Clear out the last vrfRegMsg batch
    iosxrslVrf_->vrf_msg.clear_vrfregmsgs();

    for ( const auto &routeDb_data : routeDb_ ) {
        // Create a fresh SLVrfRegMsg batch for cleanup
        iosxrslVrf_->vrfRegMsgAdd(routeDb_data.second.vrfData.vrfName);
    }
    iosxrslVrf_->unregisterVrf(AF_INET);
    iosxrslVrf_->unregisterVrf(AF_INET6); 

    asynchandler_->Shutdown();
}

std::string 
IosxrslRshuttle::iosxrIfName(std::string ifname)
{
    if (ifname == "enp0s8") {
        return "GigabitEthernet0/0/0/0";
    } else if (ifname == "enp0s9") {
        return "GigabitEthernet0/0/0/1";
    } else if (ifname == "enp0s10") {
        return "GigabitEthernet0/0/0/2";
    }

    enum IfNameTypes
    {
        GIG,
        TEN_GIG,
        FORTY_GIG,
        TWENTY_FIVE_GIG,
        HUNDRED_GIG,
        MGMT
    };

    std::map<std::string, IfNameTypes> 
    iosxrLnxIfname = {{"Gi",GIG}, {"Tg", TEN_GIG},
                      {"Fg",FORTY_GIG}, {"Tf", TWENTY_FIVE_GIG},
                      {"Hg",HUNDRED_GIG}, {"Mg", MGMT}};


    auto ifnamePrefix = "";

    switch (iosxrLnxIfname[ifname.substr(2)]) {
    case GIG:
        ifnamePrefix = "GigabitEthernet";
        break;
    case TEN_GIG:
        ifnamePrefix = "TenGigE";
        break;
    case FORTY_GIG:
        ifnamePrefix = "FortyGigE";
        break;
    case TWENTY_FIVE_GIG:
        ifnamePrefix = "TwentyFiveGigE";
        break;
    case HUNDRED_GIG:
        ifnamePrefix = "HundredGigE";
        break;
    case MGMT:
        ifnamePrefix = "MgmtEth";
        break;
    default:
        LOG(ERROR) << "Invalid Interface " << ifname;
        return "";    
    }

    // Finally replace _ with / in ifname suffix and concatenate the 
    // doctored prefix with it

    std::replace(ifname.begin(),
                 ifname.end(),
                 '_','/');    
    return ifnamePrefix + ifname.substr(ifname.length()-2);
}

void
IosxrslRshuttle::buildUnicastRoute(
  const folly::CIDRNetwork& prefix, const NextHops& nextHops) {

  for (const auto& nextHop : nextHops) {

    std::string xr_if_name = iosxrIfName(std::get<0>(nextHop));

    if (ifIdx == 0) {
      throw NetlinkException(folly::sformat(
          "Failed to get ifidx for interface: {}", std::get<0>(nextHop)));
    }
    route->addNextHop(ifIdx, std::get<1>(nextHop));
    VLOG(4) << "Added nextHop for prefix "
            << folly::IPAddress::networkToString(prefix) << " nexthop dev "
            << std::get<0>(nextHop) << " via " << std::get<1>(nextHop).str();
  }
  return route;
}

