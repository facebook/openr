/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MockNetlinkFibHandler.h"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/BinaryProtocol.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/AddressUtil.h>

using apache::thrift::FRAGILE;
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
        toIPAddress((*route).dest.prefixAddress), (*route).dest.prefixLength);

    auto newNextHops =
        from((*route).nexthops) | mapped([](const thrift::BinaryAddress& addr) {
          return std::make_pair(addr.ifName.value(), toIPAddress(addr));
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
    auto myPrefix =
        std::make_pair(
          toIPAddress((*prefix).prefixAddress), (*prefix).prefixLength);

    unicastRouteDb_.erase(myPrefix);
  }
}

void
MockNetlinkFibHandler::addUnicastRoutes(
    int16_t,
    std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes) {
  SYNCHRONIZED(unicastRouteDb_) {
    for (auto const& route : *routes) {
      auto prefix = std::make_pair(
          toIPAddress(route.dest.prefixAddress), route.dest.prefixLength);

      auto newNextHops =
          from(route.nexthops) | mapped([](const thrift::BinaryAddress& addr) {
            return std::make_pair(addr.ifName.value(), toIPAddress(addr));
          }) |
          as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

      unicastRouteDb_.emplace(prefix, newNextHops);
    }
  }
  SYNCHRONIZED(countAddRoutes_) {
    countAddRoutes_++;
  }
}

void
MockNetlinkFibHandler::deleteUnicastRoutes(
    int16_t,
    std::unique_ptr<std::vector<openr::thrift::IpPrefix>> prefixes) {
  SYNCHRONIZED(unicastRouteDb_) {
    for (auto const& prefix : *prefixes) {
      auto myPrefix = std::make_pair(
          toIPAddress(prefix.prefixAddress), prefix.prefixLength);

      unicastRouteDb_.erase(myPrefix);
    }
  }
  SYNCHRONIZED(countDelRoutes_) {
    countDelRoutes_++;
  }
}

void
MockNetlinkFibHandler::syncFib(
    int16_t,
    std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes) {
  SYNCHRONIZED(unicastRouteDb_) {
    VLOG(3) << "MockNetlinkFibHandler: Sync Fib.... "
            << (*routes).size() << " entries";
    unicastRouteDb_.clear();
    for (auto const& route : *routes) {
      auto prefix = std::make_pair(
          toIPAddress(route.dest.prefixAddress), route.dest.prefixLength);

      auto newNextHops =
          from(route.nexthops) | mapped([](const thrift::BinaryAddress& addr) {
            return std::make_pair(addr.ifName.value(), toIPAddress(addr));
          }) |
          as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

      unicastRouteDb_.emplace(prefix, newNextHops);
    }
  }
  SYNCHRONIZED(countSync_) {
    countSync_++;
  }
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
    for (auto const& kv : unicastRouteDb_) {
      auto const& prefix = kv.first;
      auto const& nextHops = kv.second;

      auto binaryNextHops = from(nextHops) |
          mapped([](const std::pair<std::string, folly::IPAddress>& nextHop) {
                              VLOG(2)
                                  << "mapping next-hop " << nextHop.second.str()
                                  << " dev " << nextHop.first;
                              auto binaryAddr = toBinaryAddress(nextHop.second);
                              binaryAddr.ifName = nextHop.first;
                              return binaryAddr;
                            }) |
          as<std::vector>();

      routes.emplace_back(thrift::UnicastRoute(
          apache::thrift::FRAGILE,
          thrift::IpPrefix(
              apache::thrift::FRAGILE,
              toBinaryAddress(prefix.first),
              static_cast<int16_t>(prefix.second)),
          std::move(binaryNextHops)));
    }
  }
}

int64_t
MockNetlinkFibHandler::getFibSyncCount() {
  int64_t res = 0;
  SYNCHRONIZED(countSync_) {
    res = countSync_;
  }
  return res;
}

int64_t
MockNetlinkFibHandler::getAddRoutesCount() {
  int64_t res = 0;
  SYNCHRONIZED(countAddRoutes_) {
    res = countAddRoutes_;
  }
  return res;
}

int64_t
MockNetlinkFibHandler::getDelRoutesCount() {
  int64_t res = 0;
  SYNCHRONIZED(countDelRoutes_) {
    res = countDelRoutes_;
  }
  return res;
}


void
MockNetlinkFibHandler::stop() {
  SYNCHRONIZED(unicastRouteDb_) {
    unicastRouteDb_.clear();
  }
  SYNCHRONIZED(countSync_) {
    countSync_ = 0;
  }
  SYNCHRONIZED(countAddRoutes_) {
    countAddRoutes_ = 0;
  }
  SYNCHRONIZED(countDelRoutes_) {
    countDelRoutes_ = 0;
  }
}

void
MockNetlinkFibHandler::restart() {
  // mimic the behavior of Fib agent get restarted
  SYNCHRONIZED(unicastRouteDb_) {
    LOG(INFO) << "Restarting fib agent";
    unicastRouteDb_.clear();
  }
  SYNCHRONIZED(startTime_) {
    startTime_ = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
  }
  SYNCHRONIZED(countSync_) {
    countSync_ = 0;
  }
  SYNCHRONIZED(countAddRoutes_) {
    countAddRoutes_ = 0;
  }
  SYNCHRONIZED(countDelRoutes_) {
    countDelRoutes_ = 0;
  }
}
} // namespace openr
