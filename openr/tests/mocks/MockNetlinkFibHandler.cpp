/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <glog/logging.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/tests/mocks/MockNetlinkFibHandler.h>
#include <unistd.h>

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {
MockNetlinkFibHandler::MockNetlinkFibHandler()
    : startTime_(std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count()) {
  VLOG(3) << "Building Mock NL Route Db";
}

void
MockNetlinkFibHandler::addUnicastRoute(
    int16_t, std::unique_ptr<openr::thrift::UnicastRoute> route) {
  SYNCHRONIZED(unicastRouteDb_) {
    auto prefix = std::make_pair(
        toIPAddress(*(*route).dest_ref()->prefixAddress_ref()),
        *(*route).dest_ref()->prefixLength_ref());

    auto newNextHops = from(*(*route).nextHops_ref()) |
        mapped([](const thrift::NextHopThrift& nh) {
                         return std::make_pair(
                             nh.address_ref()->ifName_ref().value(),
                             toIPAddress(*nh.address_ref()));
                       }) |
        as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

    unicastRouteDb_.emplace(prefix, newNextHops);
  }
}

void
MockNetlinkFibHandler::deleteUnicastRoute(
    int16_t, std::unique_ptr<openr::thrift::IpPrefix> prefix) {
  SYNCHRONIZED(unicastRouteDb_) {
    VLOG(3) << "Deleting routes of prefix" << toString(*prefix);
    auto myPrefix = std::make_pair(
        toIPAddress(*(*prefix).prefixAddress_ref()),
        *(*prefix).prefixLength_ref());

    unicastRouteDb_.erase(myPrefix);
  }
}

void
MockNetlinkFibHandler::addUnicastRoutes(
    int16_t, std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes) {
  SYNCHRONIZED(unicastRouteDb_) {
    for (auto const& route : *routes) {
      auto prefix = std::make_pair(
          toIPAddress(*route.dest_ref()->prefixAddress_ref()),
          *route.dest_ref()->prefixLength_ref());

      auto newNextHops = from(*route.nextHops_ref()) |
          mapped([](const thrift::NextHopThrift& nh) {
                           return std::make_pair(
                               nh.address_ref()->ifName_ref().value_or("none"),
                               toIPAddress(*nh.address_ref()));
                         }) |
          as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

      unicastRouteDb_.emplace(prefix, newNextHops);
    }
  }
  addRoutesCount_ += routes->size();
  updateUnicastRoutesBaton_.post();
}

void
MockNetlinkFibHandler::deleteUnicastRoutes(
    int16_t, std::unique_ptr<std::vector<openr::thrift::IpPrefix>> prefixes) {
  SYNCHRONIZED(unicastRouteDb_) {
    for (auto const& prefix : *prefixes) {
      auto myPrefix = std::make_pair(
          toIPAddress(*prefix.prefixAddress_ref()), *prefix.prefixLength_ref());

      unicastRouteDb_.erase(myPrefix);
    }
  }
  delRoutesCount_ += prefixes->size();
  deleteUnicastRoutesBaton_.post();
}

void
MockNetlinkFibHandler::syncFib(
    int16_t, std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes) {
  SYNCHRONIZED(unicastRouteDb_) {
    VLOG(3) << "MockNetlinkFibHandler: Sync Fib.... " << (*routes).size()
            << " entries";
    unicastRouteDb_.clear();
    for (auto const& route : *routes) {
      auto prefix = std::make_pair(
          toIPAddress(*route.dest_ref()->prefixAddress_ref()),
          *route.dest_ref()->prefixLength_ref());

      auto newNextHops = from(*route.nextHops_ref()) |
          mapped([](const thrift::NextHopThrift& nh) {
                           return std::make_pair(
                               nh.address_ref()->ifName_ref().value_or("none"),
                               toIPAddress(*nh.address_ref()));
                         }) |
          as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

      unicastRouteDb_.emplace(prefix, newNextHops);
    }
  }
  ++fibSyncCount_;
  syncFibBaton_.post();
}

void
MockNetlinkFibHandler::addMplsRoutes(
    int16_t, std::unique_ptr<std::vector<openr::thrift::MplsRoute>> routes) {
  SYNCHRONIZED(mplsRouteDb_) {
    for (auto& route : *routes) {
      mplsRouteDb_[*route.topLabel_ref()] = std::move(*route.nextHops_ref());
    }
  }
  addMplsRoutesCount_ += routes->size();
  updateMplsRoutesBaton_.post();
}

void
MockNetlinkFibHandler::deleteMplsRoutes(
    int16_t, std::unique_ptr<std::vector<int32_t>> labels) {
  SYNCHRONIZED(mplsRouteDb_) {
    for (auto& label : *labels) {
      mplsRouteDb_.erase(label);
    }
  }
  delMplsRoutesCount_ += labels->size();
  deleteMplsRoutesBaton_.post();
}

void
MockNetlinkFibHandler::syncMplsFib(
    int16_t, std::unique_ptr<std::vector<openr::thrift::MplsRoute>> routes) {
  SYNCHRONIZED(mplsRouteDb_) {
    mplsRouteDb_.clear();
    for (auto& route : *routes) {
      mplsRouteDb_[*route.topLabel_ref()] = std::move(*route.nextHops_ref());
    }
  }
  ++fibMplsSyncCount_;
  syncMplsFibBaton_.post();
}

thrift::SwitchRunState
MockNetlinkFibHandler::getSwitchRunState() {
  return switchState_;
}

int64_t
MockNetlinkFibHandler::aliveSince() {
  int64_t res = 0;
  SYNCHRONIZED(startTime_) {
    res = startTime_;
  }
  return res;
}

void
MockNetlinkFibHandler::getRouteTableByClient(
    std::vector<openr::thrift::UnicastRoute>& routes, int16_t) {
  SYNCHRONIZED(unicastRouteDb_) {
    routes.clear();
    VLOG(2) << "MockNetlinkFibHandler: get route table by client";
    for (auto const& [prefix, nhs] : unicastRouteDb_) {
      auto thriftNextHops =
          from(nhs) |
          mapped([](const std::pair<std::string, folly::IPAddress>& nextHop) {
            VLOG(2) << "mapping next-hop " << nextHop.second.str() << " dev "
                    << nextHop.first;
            thrift::NextHopThrift thriftNextHop;
            *thriftNextHop.address_ref() = toBinaryAddress(nextHop.second);
            thriftNextHop.address_ref()->ifName_ref() = nextHop.first;
            return thriftNextHop;
          }) |
          as<std::vector>();

      thrift::UnicastRoute route;
      route.dest_ref() = toIpPrefix(prefix);
      route.nextHops_ref() = std::move(thriftNextHops);
      routes.emplace_back(std::move(route));
    }
  }
}

void
MockNetlinkFibHandler::getMplsRouteTableByClient(
    std::vector<openr::thrift::MplsRoute>& routes, int16_t) {
  SYNCHRONIZED(mplsRouteDb_) {
    routes.clear();
    for (auto const& [topLabel, nhs] : mplsRouteDb_) {
      thrift::MplsRoute route;
      route.topLabel_ref() = topLabel;
      route.nextHops_ref() = nhs;
      routes.emplace_back(std::move(route));
    }
  }
}

void
MockNetlinkFibHandler::waitForUpdateUnicastRoutes() {
  updateUnicastRoutesBaton_.wait();
  updateUnicastRoutesBaton_.reset();
};

void
MockNetlinkFibHandler::waitForDeleteUnicastRoutes() {
  deleteUnicastRoutesBaton_.wait();
  deleteUnicastRoutesBaton_.reset();
}

void
MockNetlinkFibHandler::waitForSyncFib() {
  syncFibBaton_.wait();
  syncFibBaton_.reset();
}

void
MockNetlinkFibHandler::waitForUpdateMplsRoutes() {
  updateMplsRoutesBaton_.wait();
  updateMplsRoutesBaton_.reset();
};

void
MockNetlinkFibHandler::waitForDeleteMplsRoutes() {
  deleteMplsRoutesBaton_.wait();
  deleteMplsRoutesBaton_.reset();
}

void
MockNetlinkFibHandler::waitForSyncMplsFib() {
  syncMplsFibBaton_.wait();
  syncMplsFibBaton_.reset();
}

void
MockNetlinkFibHandler::stop() {
  SYNCHRONIZED(unicastRouteDb_) {
    unicastRouteDb_.clear();
  }
  fibSyncCount_ = 0;
  addRoutesCount_ = 0;
  delRoutesCount_ = 0;
  fibMplsSyncCount_ = 0;
  addMplsRoutesCount_ = 0;
  delMplsRoutesCount_ = 0;
}

void
MockNetlinkFibHandler::restart() {
  // mimic the behavior of Fib agent get restarted
  LOG(INFO) << "Restarting fib agent";
  unicastRouteDb_->clear();
  mplsRouteDb_->clear();

  SYNCHRONIZED(startTime_) {
    startTime_ = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
  }
  fibSyncCount_ = 0;
  addRoutesCount_ = 0;
  delRoutesCount_ = 0;
  fibMplsSyncCount_ = 0;
  addMplsRoutesCount_ = 0;
  delMplsRoutesCount_ = 0;
}

} // namespace openr
