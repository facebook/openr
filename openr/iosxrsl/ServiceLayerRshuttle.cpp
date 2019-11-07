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
} //anonymous namespace


namespace openr {

std::string grpc_connectivity_state_name(grpc_connectivity_state state) {
  switch (state) {
    case GRPC_CHANNEL_IDLE:
      return "IDLE";
    case GRPC_CHANNEL_CONNECTING:
      return "CONNECTING";
    case GRPC_CHANNEL_READY:
      return "READY";
    case GRPC_CHANNEL_TRANSIENT_FAILURE:
      return "TRANSIENT_FAILURE";
    case GRPC_CHANNEL_SHUTDOWN:
      return "SHUTDOWN";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}


VrfData::VrfData(std::string vrf_name,
            uint8_t admin_distance,
            unsigned int purge_interval)
    : vrfName(vrf_name), 
      adminDistance(admin_distance),
      vrfPurgeIntervalSeconds(purge_interval) {}

VrfData::VrfData(const VrfData &vrfData)
{
    vrfName = vrfData.vrfName;
    adminDistance = vrfData.adminDistance;
    vrfPurgeIntervalSeconds = 
            vrfData.vrfPurgeIntervalSeconds;
}


RouteDbVrf::RouteDbVrf(VrfData vrf_data)
    : vrfData(vrf_data) {}


IosxrslRshuttle::IosxrslRshuttle(
            fbzmq::ZmqEventLoop* zmqEventLoop,
            std::vector<VrfData> vrfSet,
            uint8_t routeProtocolId,
            std::shared_ptr<grpc::Channel> Channel)
  : routeProtocolId_(routeProtocolId)
{
    evl_ = zmqEventLoop;
    CHECK(evl_) << "Invalid ZMQ event loop handle";

    // Let's set up the AsyncNotifChannel to establish
    // connection to IOS-XR Service Layer 
   // asynchandler_ = std::make_unique<AsyncNotifChannel>(Channel);


    // Acquire the init lock, register vrf(s) only after condition variable is activated
    std::unique_lock<std::mutex> initlock(init_mutex);
    
   // service_layer::SLInitMsg init_msg;
   // init_msg.set_majorver(service_layer::SL_MAJOR_VERSION);
  //  init_msg.set_minorver(service_layer::SL_MINOR_VERSION);
   // init_msg.set_subver(service_layer::SL_SUB_VERSION);

    if(!spawnAsyncInitThread(Channel)) {
        LOG(ERROR) << "Failed to spawn Async-Init thread for Service Layer";
    }

    //asynchandler_ = std::make_unique<AsyncNotifChannel>(Channel);
    //notifThread_ = std::thread(&AsyncNotifChannel::AsyncCompleteRpc, asynchandler_);

    //Populate the init_msg using version VERSION fields in service_layer
    //service_layer::SLInitMsg init_msg;
    //init_msg.set_majorver(service_layer::SL_MAJOR_VERSION);
    //init_msg.set_minorver(service_layer::SL_MINOR_VERSION);
    //init_msg.set_subver(service_layer::SL_SUB_VERSION);

    //VLOG(2) << "Send init msg";
    //asynchandler_->SendInitMsg(init_msg);

    std::chrono::seconds delay(10);

    //VLOG(2) << "notif thread spawned";
    // Spawn reader thread that maintains our Notification Channel
    //notifThread_ = std::thread(&AsyncNotifChannel::AsyncCompleteRpc, asynchandler_);

    //VLOG(2) << "grpc channel state: "
      //      << grpc_connectivity_state_name(Channel->GetState(false));

   // VLOG(2) << "Send init msg";
   // asynchandler_->SendInitMsg(init_msg);

   // VLOG(2) << "init_success: " << init_success;

    // Wait on the mutex lock
    while (!init_success) {
        
        VLOG(2) << "grpc channel state: "
                << grpc_connectivity_state_name(Channel->GetState(false));

        VLOG(2) << "Wait for conditional variable for 10 seconds";
        // Wait for 10 (timeout) seconds before retrying
        init_condVar.wait_for(initlock, delay);


        if (grpc_connectivity_state_name(Channel->GetState(false)) != "READY") {
            VLOG(2) << "GRPC channel not in READY state yet";
            if (!init_success) {
                if(!cleanupAsyncInitThread()) {
                    LOG(ERROR) << "Failed to clean up existing AsyncInit Thread";
                }

                if(!spawnAsyncInitThread(Channel)) {
                    LOG(ERROR) << "Failed to spawn a new AsyncInit Thread";
                }
           /* try{
            // Shutdown the existing asynchandler_
             //   if (!asynchandler_) {
                    VLOG(4) << "Shutting down the *asynchandler_ object";
                    asynchandler_->Shutdown();
                    if (notifThread_.joinable()) {
                        VLOG(4) << "Joining notifThread_";
                        notifThread_.join();
                   }
                   VLOG(4) << "Clean up the asynchandler_ pointer";
                  asynchandler_.reset();
              // }
            } catch (IosxrslException const& ex) {
              LOG(ERROR) << "Error cleaning up Async Init object and thread";
              LOG(ERROR) << ex.what();
          } catch (std::exception const& ex) {
              LOG(ERROR) << "Error cleaning up Async Init object and thread";
              LOG(ERROR) << ex.what();
           }
            
            asynchandler_ = std::make_unique<AsyncNotifChannel>(Channel);
            notifThread_ = std::thread(&AsyncNotifChannel::AsyncCompleteRpc, asynchandler_);
      
            VLOG(2) << "Send init msg";
            asynchandler_->SendInitMsg(init_msg);

            //VLOG(2) << "spawning thread again";
            //asynchandler_ = std::make_unique<AsyncNotifChannel>(Channel);
           // notifThread_ = std::thread(&AsyncNotifChannel::AsyncCompleteRpc, asynchandler_);
          
            //VLOG(2) << "Send init msg again";
            //asynchandler_->SendInitMsg(init_msg);*/
           }
        }
    }


    VLOG(2) << "grpc channel state: "
            << grpc_connectivity_state_name(Channel->GetState(false));

    iosxrslVrf_ = std::make_unique<IosxrslVrf>(Channel);
    iosxrslRoute_ = std::make_unique<IosxrslRoute>(Channel);

    evl_->runInEventLoop([this, vrfSet]() mutable {
        for (auto const &vrf_data : vrfSet) {
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

    //asynchandler_->Shutdown();

    //notifThread_.join();

    if(!cleanupAsyncInitThread()) {
        LOG(ERROR) << "Failed to clean up the Async Init Thread in final clean up";
    }
}

bool
IosxrslRshuttle::cleanupAsyncInitThread() 
{
    //folly::Promise<folly::Unit> promise;
   // auto future = promise.getFuture();

   //evl_->runInEventLoop(
    //    [this, promise = std::move(promise), 
      //     asynchandler = std::move(asynchandler_), 
        //     notifThread = std::move(notifThread_)]() mutable {
      try{
        // Shutdown the existing asynchandler_
        //if (!asynchandler_) {
            VLOG(4) << "Shutting down the *asynchandler_ object";
            asynchandler_->Shutdown();
            if (notifThread_.joinable()) {
                VLOG(4) << "Joining notifThread_";
                notifThread_.join();
            }
            VLOG(4) << "Clean up the asynchandler_ pointer";
            asynchandler_.reset();    
        //}
      } catch (IosxrslException const& ex) {
          LOG(ERROR) << "Error cleaning up Async Init object and thread";
          LOG(ERROR) << ex.what();
          return false;
      } catch (std::exception const& ex) {
          LOG(ERROR) << "Error cleaning up Async Init object and thread";
          LOG(ERROR) << ex.what();
          return false;
      }

    return true; 
}

bool
IosxrslRshuttle::spawnAsyncInitThread(std::shared_ptr<grpc::Channel> Channel)
{
    //folly::Promise<folly::Unit> promise;
    //auto future = promise.getFuture();

   // evl_->runInEventLoop(
    //    [this, promise = std::move(promise), 
      //      asynchandler = std::move(asynchandler_), 
        //      notifThread = std::move(notifThread_),
          //      Channel]() mutable {
      VLOG(2) << "Setting up a fresh asynchandler object and notification thread";
      try {
          asynchandler_ = std::make_unique<AsyncNotifChannel>(Channel);
          notifThread_ = std::thread(&AsyncNotifChannel::AsyncCompleteRpc, asynchandler_);

          //Populate the init_msg using version VERSION fields in service_layer
          service_layer::SLInitMsg init_msg;
          init_msg.set_majorver(service_layer::SL_MAJOR_VERSION);
          init_msg.set_minorver(service_layer::SL_MINOR_VERSION);
          init_msg.set_subver(service_layer::SL_SUB_VERSION);

          VLOG(2) << "Send init msg";
          asynchandler_->SendInitMsg(init_msg);
      } catch (IosxrslException const& ex) {
         LOG(ERROR) << "Failed to set up asynchandler and notification thread";
         LOG(ERROR) << ex.what();
         return false;
      } catch (std::exception const& ex) {
         LOG(ERROR) << "Failed to set up asynchandler and notification thread";
         LOG(ERROR) << ex.what();
         return false;
     }
    return true;
}


// Set VRF context for V4 routes
void 
IosxrslRshuttle::setIosxrslRouteVrfV4(std::string vrfName)
{
    iosxrslRoute_->setVrfV4(vrfName);
}

// Set VRF context for V6 routes
void 
IosxrslRshuttle::setIosxrslRouteVrfV6(std::string vrfName)
{
    iosxrslRoute_->setVrfV6(vrfName);
}

// Set VRF context for V4 and V6 routes
void 
IosxrslRshuttle::setIosxrslRouteVrf(std::string vrfName)
{
    setIosxrslRouteVrfV4(vrfName);
    setIosxrslRouteVrfV6(vrfName);
}

std::string 
IosxrslRshuttle::iosxrIfName(std::string ifname)
{
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

    if (iosxrLnxIfname.find(ifname.substr(0,2)) ==
            iosxrLnxIfname.end()) {
        LOG(ERROR) << "Interface type not supported";
        return "";
    }

    switch (iosxrLnxIfname[ifname.substr(0,2)]) {
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


    return ifnamePrefix + ifname.substr(2, ifname.length());
}


folly::Future<folly::Unit>
IosxrslRshuttle::addUnicastRoute(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  VLOG(3) << "Adding unicast route";
  CHECK(not nextHops.empty());
  CHECK(not prefix.first.isMulticast() && not prefix.first.isLinkLocal());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runInEventLoop(
      [this, promise = std::move(promise), prefix, nextHops]() mutable {
        try {
          // In IOS-XR SL-API, we have the UPDATE utility
          // So we do NOT need to check for prefix existence
          // in current RIB. UPDATE will create a new route if 
          // it doesn't exist.
          doAddUnicastRoute(prefix, nextHops);
          promise.setValue();
        } catch (IosxrslException const& ex) {
          LOG(ERROR) << "Error adding unicast routes to "
                     << folly::IPAddress::networkToString(prefix);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error adding unicast routes to "
                     << folly::IPAddress::networkToString(prefix);
          promise.setException(ex);
        }
      });
  return future;
}

folly::Future<folly::Unit>
IosxrslRshuttle::deleteUnicastRoute(const folly::CIDRNetwork& prefix) {
  VLOG(3) << "Deleting unicast route";
  CHECK(not prefix.first.isMulticast() && not prefix.first.isLinkLocal());

  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runInEventLoop([this, promise = std::move(promise), prefix]() mutable {
    try {
/*      if (unicastRouteDb_.count(prefix) == 0) {
        LOG(ERROR) << "Trying to delete non-existing prefix "
                   << folly::IPAddress::networkToString(prefix);
      } else { */
//        const auto& oldNextHops = unicastRouteDb_.at(prefix);
        doDeleteUnicastRoute(prefix);
//        unicastRouteDb_.erase(prefix);
//      }
      promise.setValue();
    } catch (IosxrslException const& ex) {
      LOG(ERROR) << "Error deleting unicast routes to "
                 << folly::IPAddress::networkToString(prefix)
                 << " Error: " << folly::exceptionStr(ex);
      promise.setException(ex);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error deleting unicast routes to "
                 << folly::IPAddress::networkToString(prefix)
                 << " Error: " << folly::exceptionStr(ex);
      promise.setException(ex);
    }
  });
  return future;
}


folly::Future<UnicastRoutes>
IosxrslRshuttle::getUnicastRoutes() {
  VLOG(3) << "Getting all routes";

  folly::Promise<UnicastRoutes> promise;
  auto future = promise.getFuture();

  evl_->runInEventLoop([this, promise = std::move(promise)]() mutable {
    try {
        promise.setValue(doGetUnicastRoutes());
    } catch (IosxrslException const& ex) {
      LOG(ERROR) << "Error updating route cache: " << folly::exceptionStr(ex);
      promise.setException(ex);
    } catch (std::exception const& ex) {
      LOG(ERROR) << "Error updating route cache: " << folly::exceptionStr(ex);
      promise.setException(ex);
    }
  });
  return future;
}

UnicastRoutes
IosxrslRshuttle::doGetUnicastRoutes() {
    // Combine the v4 and v6 maps before returning
    if (iosxrslRoute_->routev4_msg.vrfname().empty() ||
        iosxrslRoute_->routev6_msg.vrfname().empty()) {
        throw IosxrslException(folly::sformat(
            "VRF not set, could not fetch routes, Service Layer Error: {}",
            std::to_string(service_layer::SLErrorStatus_SLErrno_SL_RPC_ROUTE_VRF_NAME_MISSING)));
    }

    auto unicastRouteDb_ = routeDb_[iosxrslRoute_->routev4_msg.vrfname()].unicastRoutesV4_;

    unicastRouteDb_.insert(routeDb_[iosxrslRoute_->routev6_msg.vrfname()].unicastRoutesV6_.begin(),
                           routeDb_[iosxrslRoute_->routev6_msg.vrfname()].unicastRoutesV6_.end());

    return unicastRouteDb_;
}

folly::Future<folly::Unit>
IosxrslRshuttle::syncRoutes(UnicastRoutes newRouteDb) {
  VLOG(3) << "Syncing Routes....";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();

  evl_->runInEventLoop(
      [this, promise = std::move(promise), newRouteDb]() mutable {
        try {
          doSyncRoutes(newRouteDb);
          promise.setValue();
        } catch (IosxrslException const& ex) {
          LOG(ERROR) << "Error syncing routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        } catch (std::exception const& ex) {
          LOG(ERROR) << "Error syncing routeDb with Fib: "
                     << folly::exceptionStr(ex);
          promise.setException(ex);
        }
      });
  return future;
}

void
IosxrslRshuttle::doAddUnicastRoute(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  if (prefix.first.isV4()) {
    if(iosxrslRoute_->
         routev4_msg.vrfname().empty()) {
        throw IosxrslException(folly::sformat(
            "Could not add Route to: {} Service Layer Error: {}",
            folly::IPAddress::networkToString(prefix),
            std::to_string(service_layer::SLErrorStatus_SLErrno_SL_RPC_ROUTE_VRF_NAME_MISSING)));
    }
    routeDb_[iosxrslRoute_->
               routev4_msg.vrfname()].unicastRoutesV4_[prefix] = nextHops; 
    doAddUnicastRouteV4(prefix, nextHops);
  } else {
    if(iosxrslRoute_->
         routev6_msg.vrfname().empty()) {
        throw IosxrslException(folly::sformat(
            "Could not add Route to: {} Service Layer Error: {}",
            folly::IPAddress::networkToString(prefix),
            std::to_string(service_layer::SLErrorStatus_SLErrno_SL_RPC_ROUTE_VRF_NAME_MISSING)));
    }
    routeDb_[iosxrslRoute_->
               routev6_msg.vrfname()].unicastRoutesV6_[prefix] = nextHops;
    doAddUnicastRouteV6(prefix, nextHops);
  }

  // Cache new nexthops in our local-cache if everything is good
//  unicastRoutes_[prefix].insert(nextHops.begin(), nextHops.end());
}


void
IosxrslRshuttle::doAddUnicastRouteV4(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  CHECK(prefix.first.isV4());

  VLOG(3) << "Prefix Received: " << folly::IPAddress::networkToString(prefix);

  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.second.isV4());
    VLOG(3) << "Nexthop : "<< std::get<1>(nextHop).str() << ", " << std::get<0>(nextHop).c_str();
    // Create a path list

    auto nexthop_if = iosxrIfName(std::get<0>(nextHop));
    auto nexthop_address = std::get<1>(nextHop).str();

    iosxrslRoute_->insertAddBatchV4(prefix.first.str(),
                                    folly::to<uint8_t>(prefix.second),
                                    routeProtocolId_,
                                    nexthop_address,
                                    nexthop_if);
  }

  // Using the Update Operation to replace an existing prefix
  // or create one if it doesn't exist.
  auto result  = iosxrslRoute_->routev4Op(service_layer::SL_OBJOP_UPDATE);
  if (!result) {
    throw IosxrslException(folly::sformat(
        "Could not add Route to: {}",
        folly::IPAddress::networkToString(prefix)));
  }
}

void
IosxrslRshuttle::doAddUnicastRouteV6(
    const folly::CIDRNetwork& prefix, const NextHops& nextHops) {
  CHECK(prefix.first.isV6());
  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.second.isV6());
  }

  VLOG(3) << "Prefix Received: " << folly::IPAddress::networkToString(prefix);

  for (auto const& nextHop : nextHops) {
    CHECK(nextHop.second.isV6());
    VLOG(3) << "Nexthop : "<< std::get<1>(nextHop).str() << ", " << std::get<0>(nextHop).c_str();
    // Create a path list

    auto nexthop_if = iosxrIfName(std::get<0>(nextHop));
    auto nexthop_address = std::get<1>(nextHop).str();

    iosxrslRoute_->insertAddBatchV6(prefix.first.str(),
                                    folly::to<uint8_t>(prefix.second),
                                    routeProtocolId_,
                                    nexthop_address,
                                    nexthop_if);
  }

  // Using the Update Operation to replace an existing prefix
  // or create one if it doesn't exist.
  auto result  = iosxrslRoute_->routev6Op(service_layer::SL_OBJOP_UPDATE);
  if (!result) {
    throw IosxrslException(folly::sformat(
        "Could not add Route to: {}",
        folly::IPAddress::networkToString(prefix)));
  }

}

void
IosxrslRshuttle::doDeleteUnicastRoute(
                    const folly::CIDRNetwork& prefix) {
  if (prefix.first.isV4()) {
    if(iosxrslRoute_->
         routev4_msg.vrfname().empty()) {
        throw IosxrslException(folly::sformat(
            "Could not delete prefix: {} Service Layer Error: {}",
            folly::IPAddress::networkToString(prefix),
            std::to_string(service_layer::SLErrorStatus_SLErrno_SL_RPC_ROUTE_VRF_NAME_MISSING)));
    }
    routeDb_[iosxrslRoute_->
               routev4_msg.vrfname()].unicastRoutesV4_.erase(prefix);
    deleteUnicastRouteV4(prefix);
  } else {
    if(iosxrslRoute_->
         routev6_msg.vrfname().empty()) {
        throw IosxrslException(folly::sformat(
            "Could not delete prefix: {} Service Layer Error: {}",
            folly::IPAddress::networkToString(prefix),
            std::to_string(service_layer::SLErrorStatus_SLErrno_SL_RPC_ROUTE_VRF_NAME_MISSING)));
    }
    routeDb_[iosxrslRoute_->
               routev6_msg.vrfname()].unicastRoutesV6_.erase(prefix);

    deleteUnicastRouteV6(prefix);
  }

}


void
IosxrslRshuttle::deleteUnicastRouteV4(
                  const folly::CIDRNetwork& prefix) {
  CHECK(prefix.first.isV4());

  iosxrslRoute_->insertDeleteBatchV4(prefix.first.str(),
                                  folly::to<uint8_t>(prefix.second));


  auto result = iosxrslRoute_->routev4Op(service_layer::SL_OBJOP_DELETE);

  if (!result) {
    throw IosxrslException(folly::sformat(
        "Failed to delete route {}",
        folly::IPAddress::networkToString(prefix)));
  }
}

void
IosxrslRshuttle::deleteUnicastRouteV6(
                  const folly::CIDRNetwork& prefix) {
  CHECK(prefix.first.isV6());

  iosxrslRoute_->insertDeleteBatchV6(prefix.first.str(),
                                  folly::to<uint8_t>(prefix.second));


  auto result = iosxrslRoute_->routev6Op(service_layer::SL_OBJOP_DELETE);

  if (!result) {
    throw IosxrslException(folly::sformat(
        "Failed to delete route {}",
        folly::IPAddress::networkToString(prefix)));
  }


}


void
IosxrslRshuttle::doSyncRoutes(UnicastRoutes newRouteDb) {

  // Fetch the latest Application RIB state
  auto unicastRouteDb_ = doGetUnicastRoutes();

  // Go over routes that are not in new routeDb, delete
  for (auto it = unicastRouteDb_.begin(); it != unicastRouteDb_.end();) {
    auto const& prefix = it->first;
    if (newRouteDb.find(prefix) == newRouteDb.end()) {
      try {
        doDeleteUnicastRoute(prefix);
      } catch (IosxrslException const& err) {
        LOG(ERROR) << folly::sformat(
            "Could not del Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err));
      } catch (std::exception const& err) {
        LOG(ERROR) << folly::sformat(
            "Could not del Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err));
      }
      it = unicastRouteDb_.erase(it);
    } else {
      ++it;
    }
  }

  // Using the Route batch UPDATE utility in IOSXR SL-API,
  // simply push the newRoutedb into the XR RIB

  for (auto const& kv : newRouteDb) {
      auto const& prefix = kv.first;
      try {
        doAddUnicastRoute(prefix, kv.second);
      } catch (IosxrslException const& err) {
        LOG(ERROR) << folly::sformat(
            "Could not add Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err));
      } catch (std::exception const& err) {
        LOG(ERROR) << folly::sformat(
            "Could not add Route to: {} Error: {}",
            folly::IPAddress::networkToString(prefix),
            folly::exceptionStr(err));
      }
      unicastRouteDb_.emplace(prefix, std::move(kv.second));
  }

}


}
