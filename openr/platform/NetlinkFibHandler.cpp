/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkFibHandler.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/gen/Base.h>
#include <folly/logging/xlog.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Platform_constants.h>
#include <openr/platform/NetlinkFibHandler.h>

namespace openr {

namespace {

// iproute2 protocol IDs in the kernel are a shared resource
// Various well known and custom protocols use it
// This is a *Weak* attempt to protect against some already
// known protocols
const uint8_t kMinRouteProtocolId = 17;
const uint8_t kMaxRouteProtocolId = 253;

template <typename T>
folly::SemiFuture<T>
createSemiFutureWithClientIdError() {
  auto [p, sf] = folly::makePromiseContract<T>();
  p.setException(fbnl::NlException("Invalid clientId or protocol mapping"));
  return std::move(sf);
}

} // namespace

NetlinkFibHandler::NetlinkFibHandler(fbnl::NetlinkProtocolSocket* nlSock)
    : facebook::fb303::BaseService("openr"),
      nlSock_(nlSock),
      startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count()) {
  CHECK_NOTNULL(nlSock);
}

NetlinkFibHandler::~NetlinkFibHandler() {}

std::optional<int16_t>
NetlinkFibHandler::getProtocol(int16_t clientId) {
  auto ret = thrift::Platform_constants::clientIdtoProtocolId().find(clientId);
  if (ret == thrift::Platform_constants::clientIdtoProtocolId().end()) {
    return std::nullopt;
  }
  if (ret->second < kMinRouteProtocolId || ret->second > kMaxRouteProtocolId) {
    return std::nullopt;
  }

  return ret->second;
}

std::string
NetlinkFibHandler::getClientName(const int16_t clientId) {
  return apache::thrift::util::enumNameSafe(
      static_cast<thrift::FibClient>(clientId));
}

uint8_t
NetlinkFibHandler::protocolToPriority(const uint8_t protocol) {
  // Lookup in protocol to priority mapping
  auto& priorityMap = openr::thrift::Platform_constants::protocolIdtoPriority();
  auto priorityIt = priorityMap.find(protocol);
  if (priorityIt != priorityMap.end()) {
    return priorityIt->second;
  }

  // Default priority is unknown
  return openr::thrift::Platform_constants::kUnknowProtAdminDistance();
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_addUnicastRoute(
    int16_t clientId, std::unique_ptr<thrift::UnicastRoute> route) {
  auto routes = std::make_unique<std::vector<thrift::UnicastRoute>>();
  routes->emplace_back(std::move(*route));
  return semifuture_addUnicastRoutes(clientId, std::move(routes));
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_deleteUnicastRoute(
    int16_t clientId, std::unique_ptr<thrift::IpPrefix> prefix) {
  auto prefixes = std::make_unique<std::vector<thrift::IpPrefix>>();
  prefixes->emplace_back(std::move(*prefix));
  return semifuture_deleteUnicastRoutes(clientId, std::move(prefixes));
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_addUnicastRoutes(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> routes) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<folly::Unit>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Adding/Updating unicast routes of client "
             << getClientName(clientId) << ", numRoutes=" << routes->size();

  // Add routes and return a collected semifuture
  std::vector<folly::SemiFuture<int>> result;
  for (auto& route : *routes) {
    result.emplace_back(nlSock_->addRoute(buildRoute(route, protocol.value())));
  }
  return fbnl::NetlinkProtocolSocket::collectReturnStatus(
      std::move(result), {EEXIST});
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_deleteUnicastRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::IpPrefix>> prefixes) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<folly::Unit>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Deleting unicast routes of client " << getClientName(clientId)
             << ", numRoutes=" << prefixes->size();

  // Delete routes and return a collected semifuture
  std::vector<folly::SemiFuture<int>> result;
  for (auto& prefix : *prefixes) {
    fbnl::RouteBuilder rtBuilder;
    rtBuilder.setDestination(toIPNetwork(prefix));
    rtBuilder.setProtocolId(protocol.value());
    result.emplace_back(nlSock_->deleteRoute(rtBuilder.build()));
  }
  return fbnl::NetlinkProtocolSocket::collectReturnStatus(
      std::move(result), {ESRCH});
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_addMplsRoutes(
    int16_t clientId, std::unique_ptr<std::vector<thrift::MplsRoute>> routes) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<folly::Unit>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Adding/Updating mpls routes of client "
             << getClientName(clientId) << ", numRoutes=" << routes->size();

  // Add routes and return a collected semifuture
  std::vector<folly::SemiFuture<int>> result;
  for (auto& route : *routes) {
    result.emplace_back(
        nlSock_->addRoute(buildMplsRoute(route, protocol.value())));
  }
  return fbnl::NetlinkProtocolSocket::collectReturnStatus(
      std::move(result), {EEXIST});
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_deleteMplsRoutes(
    int16_t clientId, std::unique_ptr<std::vector<int32_t>> topLabels) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<folly::Unit>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Deleting mpls routes of client " << getClientName(clientId)
             << ", numRoutes=" << topLabels->size();

  // Delete routes and return a collected semifuture
  std::vector<folly::SemiFuture<int>> result;
  for (auto& topLabel : *topLabels) {
    fbnl::RouteBuilder rtBuilder;
    rtBuilder.setMplsLabel(topLabel);
    rtBuilder.setProtocolId(protocol.value());
    result.emplace_back(nlSock_->deleteRoute(rtBuilder.build()));
  }
  return fbnl::NetlinkProtocolSocket::collectReturnStatus(
      std::move(result), {ESRCH});
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_syncFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::UnicastRoute>> unicastRoutes) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<folly::Unit>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Syncing unicast FIB for client " << getClientName(clientId)
             << ", numRoutes=" << unicastRoutes->size();

  // SemiFuture vector for collecting return values of all API calls
  std::vector<folly::SemiFuture<int>> result;

  // Create set of existing route
  // NOTE: Synchronous call to retrieve all the routes. We first make both
  // requests to retrieve IPv4 and IPv6 routes. Subsequently we wait on them
  // to complete and prepare the map of existing routes
  std::unordered_map<folly::CIDRNetwork, fbnl::Route> existingRoutes;
  {
    auto v4Routes = nlSock_->getIPv4Routes(protocol.value()).get();
    auto v6Routes = nlSock_->getIPv6Routes(protocol.value()).get();
    if (v4Routes.hasError()) {
      throw fbnl::NlException("Failed fetching IPv4 routes", v4Routes.error());
    }
    if (v6Routes.hasError()) {
      throw fbnl::NlException("Failed fetching IPv6 routes", v6Routes.error());
    }
    for (auto& routesPtr : {&v4Routes, &v6Routes}) {
      for (auto& route : routesPtr->value()) {
        const auto prefix = route.getDestination();
        // Linux will report a null next-hop for RTN_BLACKHOLE type while
        // RIB does not
        if (route.getType() == RTN_BLACKHOLE) {
          route.setNextHops({});
        }
        existingRoutes.emplace(prefix, std::move(route));
      }
    }
  }

  // Go over the new routes. Add or update
  std::unordered_set<folly::CIDRNetwork> newPrefixes;
  for (auto& route : *unicastRoutes) {
    const auto network = toIPNetwork(*route.dest_ref());
    newPrefixes.insert(network);
    auto nlRoute = buildRoute(route, protocol.value());
    auto it = existingRoutes.find(network);
    if (it != existingRoutes.end() and it->second == nlRoute) {
      // Existing route is same as the one we're trying to add. SKIP
      continue;
    }
    if (it != existingRoutes.end()) {
      XLOG(INFO) << "Updating unicast-route "
                 << "\n[OLD] " << it->second.str() << "\n[NEW] "
                 << nlRoute.str();
    } else {
      XLOG(INFO) << "Adding unicast-route \n[NEW]" << nlRoute.str();
    }
    // Add new route or replace existing one
    result.emplace_back(nlSock_->addRoute(nlRoute));
  }

  // Go over the old routes to remove stale ones
  for (auto& [prefix, nlRoute] : existingRoutes) {
    if (newPrefixes.count(prefix)) {
      // not a stale route
      continue;
    }
    // Delete stale route
    XLOG(INFO) << "Deleting unicast-route "
               << folly::IPAddress::networkToString(prefix);
    result.emplace_back(nlSock_->deleteRoute(nlRoute));
  }

  // Return collected result
  // NOTE: We're ignoring EEXIST error code. ESRCH error code must not be
  // raised because we're deleting route that already exist
  return fbnl::NetlinkProtocolSocket::collectReturnStatus(
      std::move(result), {EEXIST});
}

folly::SemiFuture<folly::Unit>
NetlinkFibHandler::semifuture_syncMplsFib(
    int16_t clientId,
    std::unique_ptr<std::vector<thrift::MplsRoute>> mplsRoutes) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<folly::Unit>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Syncing mpls FIB for client " << getClientName(clientId)
             << ", numRoutes=" << mplsRoutes->size();

  // SemiFuture vector for collecting return values of all API calls
  std::vector<folly::SemiFuture<int>> result;

  // Create set of existing route
  // NOTE: Synchronous call to retrieve all the routes
  std::unordered_map<int32_t, fbnl::Route> existingRoutes;
  auto nlRoutes = nlSock_->getMplsRoutes(protocol.value()).get();
  if (nlRoutes.hasError()) {
    throw fbnl::NlException("Failed fetching IPv6 routes", nlRoutes.error());
  }
  for (auto& route : std::move(nlRoutes).value()) {
    const auto topLabel = route.getMplsLabel().value();
    existingRoutes.emplace(topLabel, std::move(route));
  }

  // Go over the new routes. Add or update
  std::unordered_set<uint32_t> newLabels;
  for (auto& route : *mplsRoutes) {
    const auto label = *route.topLabel_ref();
    newLabels.insert(label);
    auto nlRoute = buildMplsRoute(route, protocol.value());
    auto it = existingRoutes.find(label);
    if (it != existingRoutes.end() and it->second == nlRoute) {
      // Existing route is same as the one we're trying to add. SKIP
      continue;
    }
    if (it != existingRoutes.end()) {
      XLOG(INFO) << "Updating mpls-route "
                 << "\n[OLD] " << it->second.str() << "\n[NEW] "
                 << nlRoute.str();
    } else {
      XLOG(INFO) << "Adding mpls-route \n[NEW]" << nlRoute.str();
    }
    // Add new route or replace existing one
    result.emplace_back(nlSock_->addRoute(nlRoute));
  }

  // Go over the old routes to remove stale ones
  for (auto& [topLabel, nlRoute] : existingRoutes) {
    if (newLabels.count(topLabel)) {
      // not a stale route
      continue;
    }
    // Delete stale route
    XLOG(INFO) << "Deleting mpls-route " << *nlRoute.getMplsLabel();
    result.emplace_back(nlSock_->deleteRoute(nlRoute));
  }

  // Return collected result
  return fbnl::NetlinkProtocolSocket::collectReturnStatus(
      std::move(result), {EEXIST, ESRCH});
}

int64_t
NetlinkFibHandler::aliveSince() {
  return startTime_;
}

facebook::fb303::cpp2::fb303_status
NetlinkFibHandler::getStatus() {
  return facebook::fb303::cpp2::fb303_status::ALIVE;
}

openr::thrift::SwitchRunState
NetlinkFibHandler::getSwitchRunState() {
  return openr::thrift::SwitchRunState::CONFIGURED;
}

folly::SemiFuture<std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>
NetlinkFibHandler::semifuture_getRouteTableByClient(int16_t clientId) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<
        std::unique_ptr<std::vector<openr::thrift::UnicastRoute>>>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Get unicast routes for client " << getClientName(clientId);

  auto v4Routes = nlSock_->getIPv4Routes(protocol.value());
  auto v6Routes = nlSock_->getIPv6Routes(protocol.value());
  return folly::collectAll(std::move(v4Routes), std::move(v6Routes))
      .deferValue(
          [this](std::tuple<
                 folly::Try<folly::Expected<std::vector<fbnl::Route>, int>>,
                 folly::Try<folly::Expected<std::vector<fbnl::Route>, int>>>&&
                     res) {
            auto routes = std::make_unique<std::vector<thrift::UnicastRoute>>();
            for (auto& nlRoutes : {std::get<0>(res), std::get<1>(res)}) {
              if (nlRoutes.value().hasError()) {
                throw fbnl::NlException(
                    "Failed fetching routes", nlRoutes.value().error());
              }
              for (auto& nlRoute : nlRoutes.value().value()) {
                thrift::UnicastRoute route;
                route.dest_ref() = toIpPrefix(nlRoute.getDestination());
                route.nextHops_ref() = toThriftNextHops(nlRoute.getNextHops());
                routes->emplace_back(std::move(route));
              }
            }
            return routes;
          });
}

folly::SemiFuture<std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>
NetlinkFibHandler::semifuture_getMplsRouteTableByClient(int16_t clientId) {
  const auto protocol = getProtocol(clientId);
  if (not protocol.has_value()) {
    return createSemiFutureWithClientIdError<
        std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>();
  }
  CHECK(protocol.has_value());
  XLOG(INFO) << "Get mpls routes for client " << getClientName(clientId);

  return nlSock_->getMplsRoutes(protocol.value())
      .deferValue(
          [this](folly::Expected<std::vector<fbnl::Route>, int>&& nlRoutes) {
            if (nlRoutes.hasError()) {
              throw fbnl::NlException(
                  "Failed fetching routes", nlRoutes.error());
            }
            auto routes = std::make_unique<std::vector<thrift::MplsRoute>>();
            routes->reserve(nlRoutes->size());
            for (auto& nlRoute : nlRoutes.value()) {
              thrift::MplsRoute route;
              route.topLabel_ref() = nlRoute.getMplsLabel().value();
              route.nextHops_ref() = toThriftNextHops(nlRoute.getNextHops());
              routes->emplace_back(std::move(route));
            }
            return routes;
          });
}

std::vector<thrift::NextHopThrift>
NetlinkFibHandler::toThriftNextHops(const fbnl::NextHopSet& nextHops) {
  std::vector<thrift::NextHopThrift> thriftNextHops;

  for (auto const& nh : nextHops) {
    auto labelAction = nh.getLabelAction();
    thrift::NextHopThrift nextHop;

    // Add nexthop address
    if (nh.getGateway().has_value()) {
      *nextHop.address_ref() = toBinaryAddress(nh.getGateway().value());
      // Add nexthop interface if any
      if (nh.getIfIndex().has_value()) {
        nextHop.address_ref()->ifName_ref() =
            getIfName(nh.getIfIndex().value()).value();
      }
    } else {
      // POP_AND_LOOKUP mpls nexthop has no nexthop address so we assign
      // valid but zeroed ipv6 address.
      CHECK(labelAction.has_value());
      CHECK(thrift::MplsActionCode::POP_AND_LOOKUP == labelAction.value());
      *nextHop.address_ref() = toBinaryAddress(folly::IPAddressV6("::"));
    }

    // Set nexthop weight
    nextHop.weight_ref() = nh.getWeight();

    // Add mpls action
    if (labelAction.has_value()) {
      if (labelAction.value() == thrift::MplsActionCode::POP_AND_LOOKUP ||
          labelAction.value() == thrift::MplsActionCode::PHP) {
        nextHop.mplsAction_ref() = createMplsAction(labelAction.value());
      } else if (labelAction.value() == thrift::MplsActionCode::SWAP) {
        nextHop.mplsAction_ref() =
            createMplsAction(labelAction.value(), nh.getSwapLabel().value());
      } else if (labelAction.value() == thrift::MplsActionCode::PUSH) {
        nextHop.mplsAction_ref() = createMplsAction(
            labelAction.value(), std::nullopt, nh.getPushLabels().value());
      }
    }

    thriftNextHops.emplace_back(std::move(nextHop));
  }
  return thriftNextHops;
}

void
NetlinkFibHandler::buildMplsAction(
    fbnl::NextHopBuilder& nhBuilder, const thrift::NextHopThrift& nhop) {
  if (!nhop.mplsAction_ref().has_value()) {
    return;
  }
  auto mplsAction = nhop.mplsAction_ref().value();
  nhBuilder.setLabelAction(*mplsAction.action_ref());
  if (*mplsAction.action_ref() == thrift::MplsActionCode::SWAP) {
    if (!mplsAction.swapLabel_ref().has_value()) {
      throw fbnl::NlException("Swap label not provided");
    }
    nhBuilder.setSwapLabel(mplsAction.swapLabel_ref().value());
  } else if (*mplsAction.action_ref() == thrift::MplsActionCode::PUSH) {
    if (!mplsAction.pushLabels_ref().has_value()) {
      throw fbnl::NlException("Push label(s) not provided");
    }
    nhBuilder.setPushLabels(mplsAction.pushLabels_ref().value());
  } else if (
      *mplsAction.action_ref() == thrift::MplsActionCode::POP_AND_LOOKUP) {
    auto lpbkIfIndex = getLoopbackIfIndex();
    if (lpbkIfIndex.has_value()) {
      nhBuilder.setIfIndex(lpbkIfIndex.value());
      nhBuilder.unsetGateway(); // Unset gateway address if any
    } else {
      throw fbnl::NlException("POP action, loopback interface not available");
    }
  }
  return;
}

void
NetlinkFibHandler::buildNextHop(
    fbnl::RouteBuilder& rtBuilder,
    const std::vector<thrift::NextHopThrift>& nhop) {
  // add nexthops
  fbnl::NextHopBuilder nhBuilder;
  for (const auto& nh : nhop) {
    if (nh.address_ref()->ifName_ref()) {
      nhBuilder.setIfIndex(getIfIndex(*nh.address_ref()->ifName_ref()).value());
    }
    nhBuilder.setGateway(toIPAddress(*nh.address_ref()));
    buildMplsAction(nhBuilder, nh);
    nhBuilder.setWeight(*nh.weight_ref());
    rtBuilder.addNextHop(nhBuilder.build());
    nhBuilder.reset();
  }
}

fbnl::Route
NetlinkFibHandler::buildRoute(const thrift::UnicastRoute& route, int protocol) {
  // Create route object
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setDestination(toIPNetwork(*route.dest_ref()))
      .setProtocolId(protocol)
      .setPriority(protocolToPriority(protocol))
      .setFlags(0)
      .setValid(true);

  if (route.nextHops_ref()->empty()) {
    // Empty nexthops is same as DROP (aka RTN_BLACKHOLE)
    rtBuilder.setType(RTN_BLACKHOLE);
  } else {
    // Add nexthops
    buildNextHop(rtBuilder, *route.nextHops_ref());
  }

  return rtBuilder.build();
}

fbnl::Route
NetlinkFibHandler::buildMplsRoute(
    const thrift::MplsRoute& mplsRoute, int protocol) {
  // Create route object
  // NOTE: Priority for MPLS routes is not supported in Linux
  fbnl::RouteBuilder rtBuilder;
  rtBuilder.setMplsLabel(static_cast<uint32_t>(*mplsRoute.topLabel_ref()))
      .setProtocolId(protocol)
      .setFlags(0)
      .setValid(true);

  if (mplsRoute.nextHops_ref()->empty()) {
    // Empty nexthops is same as DROP (aka RTN_BLACKHOLE)
    rtBuilder.setType(RTN_BLACKHOLE);
  } else {
    // Add nexthops
    buildNextHop(rtBuilder, *mplsRoute.nextHops_ref());
  }

  return rtBuilder.setValid(true).build();
}

std::optional<int>
NetlinkFibHandler::getIfIndex(const std::string& ifName) {
  // Lambda function to lookup ifName in cache
  auto getCachedIndex = [this, &ifName]() -> std::optional<int> {
    auto cache = ifNameToIndex_.rlock();
    auto it = cache->find(ifName);
    if (it != cache->end()) {
      return it->second;
    }
    return std::nullopt;
  };

  // Lookup in cache. Return if exists
  auto maybeIndex = getCachedIndex();
  if (maybeIndex.has_value()) {
    return maybeIndex;
  }

  // Update cache and return cached index
  initializeInterfaceCache();
  return getCachedIndex();
}

std::optional<std::string>
NetlinkFibHandler::getIfName(const int ifIndex) {
  // Lambda function to lookup ifIndex in cache
  auto getCachedName = [this, ifIndex]() -> std::optional<std::string> {
    auto cache = ifIndexToName_.rlock();
    auto it = cache->find(ifIndex);
    if (it != cache->end()) {
      return it->second;
    }
    return std::nullopt;
  };

  // Lookup in cache. Return if exists
  auto maybeName = getCachedName();
  if (maybeName.has_value()) {
    return maybeName;
  }

  // Update cache and return cached index
  initializeInterfaceCache();
  return getCachedName();
}

std::optional<int>
NetlinkFibHandler::getLoopbackIfIndex() {
  auto index = loopbackIfIndex_.load();
  if (index < 0) {
    initializeInterfaceCache();
    index = loopbackIfIndex_.load();
  }

  if (index < 0) {
    return std::nullopt;
  }
  return index;
}

void
NetlinkFibHandler::initializeInterfaceCache() noexcept {
  auto links = nlSock_->getAllLinks().get().value();

  // Acquire locks on the cache
  auto lockedIfNameToIndex = ifNameToIndex_.wlock();
  auto lockedIfIndexToName = ifIndexToName_.wlock();

  // NOTE: We don't clear cache instead override entries
  for (auto const& link : links) {
    // Update name <-> index mappings
    (*lockedIfNameToIndex)[link.getLinkName()] = link.getIfIndex();
    (*lockedIfIndexToName)[link.getIfIndex()] = link.getLinkName();

    // Update loopbackIfIndex_
    if (link.isLoopback()) {
      loopbackIfIndex_.store(link.getIfIndex());
    }
  }
}

void
NetlinkFibHandler::sendNeighborDownInfo(
    std::unique_ptr<std::vector<std::string>> neighborIps) {
  std::lock_guard<std::mutex> g(listenersMutex_);
  for (auto& listener : listeners_.accessAllThreads()) {
    XLOG(INFO) << "Sending notification to bgpD";
    listener.eventBase->runInEventBaseThread(
        [this, neighborIps = std::move(*neighborIps), listenerPtr = &listener] {
          XLOG(INFO) << "firing off notification";
          invokeNeighborListeners(listenerPtr, neighborIps, false);
        });
  }
}

void
NetlinkFibHandler::async_eb_registerForNeighborChanged(
    std::unique_ptr<apache::thrift::HandlerCallback<void>> cb) {
  auto ctx = cb->getConnectionContext()->getConnectionContext();
  auto client = ctx->getDuplexClient<
      thrift::NeighborListenerClientForFibagentAsyncClient>();

  XLOG(INFO) << "registered for bgp";
  std::lock_guard<std::mutex> g(listenersMutex_);
  auto info = listeners_.get();
  CHECK(cb->getEventBase()->isInEventBaseThread());
  if (!info) {
    info = new ThreadLocalListener(cb->getEventBase());
    listeners_.reset(info);
  }

  // make sure the eventbase is same, because later we want to run callback in
  // cb's eventbase
  DCHECK_EQ(info->eventBase, cb->getEventBase());
  if (!info->eventBase) {
    info->eventBase = cb->getEventBase();
  }
  info->clients.clear();
  info->clients.emplace(ctx, client);
  XLOG(INFO) << "registered for bgp success";
  cb->done();
}

void
NetlinkFibHandler::invokeNeighborListeners(
    ThreadLocalListener* listener,
    const std::vector<std::string>& neighborIps,
    bool isReachable) {
  // Collect the iterators to avoid erasing and potentially reordering
  // the iterators in the list.
  for (const auto& ctx : brokenClients_) {
    listener->clients.erase(ctx);
  }
  brokenClients_.clear();
  for (auto& client : listener->clients) {
    auto clientDone = [&](apache::thrift::ClientReceiveState&& state) {
      try {
        thrift::NeighborListenerClientForFibagentAsyncClient::
            recv_neighborsChanged(state);
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Exception in neighbor listener: " << ex.what();
        brokenClients_.push_back(client.first);
      }
    };

    std::vector<std::string> added, removed;
    if (isReachable) {
      added = neighborIps;
    } else {
      removed = neighborIps;
    }
    client.second->neighborsChanged(clientDone, added, removed);
  }
}

} // namespace openr
