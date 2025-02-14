/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <glog/logging.h>

#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/tests/mocks/MockNetlinkFibHandler.h>

using folly::gen::as;
using folly::gen::from;
using folly::gen::mapped;

namespace openr {
MockNetlinkFibHandler::MockNetlinkFibHandler() : startTime_(1) {
  VLOG(3) << "Building Mock NL Route Db";
}

void
MockNetlinkFibHandler::addUnicastRoute(
    int16_t, std::unique_ptr<openr::thrift::UnicastRoute> route) {
  CHECK(false) << "Not implemented & not used by Open/R";
}

void
MockNetlinkFibHandler::deleteUnicastRoute(
    int16_t, std::unique_ptr<openr::thrift::IpPrefix> prefix) {
  CHECK(false) << "Not implemented & not used by Open/R";
}

void
MockNetlinkFibHandler::addUnicastRoutes(
    int16_t, std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes) {
  ensureHealthy();

  // Acquire locks
  auto unicastRouteDb = unicastRouteDb_.wlock();
  auto dirtyPrefixes = dirtyPrefixes_.rlock();

  // Update routes
  std::vector<thrift::IpPrefix> failedPrefixes;
  for (auto const& route : *routes) {
    auto prefix = std::make_pair(
        toIPAddress(*route.dest()->prefixAddress()),
        *route.dest()->prefixLength());

    if (dirtyPrefixes->count(prefix)) {
      failedPrefixes.emplace_back(*route.dest());
      continue;
    }

    auto newNextHops = from(*route.nextHops()) |
        mapped([](const thrift::NextHopThrift& nh) {
                         return std::make_pair(
                             nh.address()->ifName().value_or("none"),
                             toIPAddress(*nh.address()));
                       }) |
        as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();
    unicastRouteDb->emplace(prefix, newNextHops);
  }
  addRoutesCount_ += routes->size() - failedPrefixes.size();
  updateUnicastRoutesBaton_.post();

  // Throw FibUpdateError if applicable
  if (failedPrefixes.size()) {
    thrift::PlatformFibUpdateError error;
    error.vrf2failedAddUpdatePrefixes()->emplace(0, std::move(failedPrefixes));
    throw error;
  }
}

void
MockNetlinkFibHandler::deleteUnicastRoutes(
    int16_t, std::unique_ptr<std::vector<openr::thrift::IpPrefix>> prefixes) {
  ensureHealthy();

  // Acquire locks
  auto unicastRouteDb = unicastRouteDb_.wlock();

  // Delete routes
  for (auto const& prefix : *prefixes) {
    auto myPrefix = std::make_pair(
        toIPAddress(*prefix.prefixAddress()), *prefix.prefixLength());

    unicastRouteDb->erase(myPrefix);
  }
  delRoutesCount_ += prefixes->size();
  deleteUnicastRoutesBaton_.post();
}

void
MockNetlinkFibHandler::syncFib(
    int16_t, std::unique_ptr<std::vector<openr::thrift::UnicastRoute>> routes) {
  ensureHealthy();

  // Acquire locks
  auto unicastRouteDb = unicastRouteDb_.wlock();
  auto dirtyPrefixes = dirtyPrefixes_.rlock();

  // Sync routes
  std::vector<thrift::IpPrefix> failedPrefixesToAdd, failedPrefixesToDelete;
  unicastRouteDb->clear();
  for (auto const& route : *routes) {
    auto prefix = std::make_pair(
        toIPAddress(*route.dest()->prefixAddress()),
        *route.dest()->prefixLength());

    if (dirtyPrefixes->count(prefix)) {
      failedPrefixesToAdd.emplace_back(*route.dest());
      continue;
    }

    auto newNextHops = from(*route.nextHops()) |
        mapped([](const thrift::NextHopThrift& nh) {
                         return std::make_pair(
                             nh.address()->ifName().value_or("none"),
                             toIPAddress(*nh.address()));
                       }) |
        as<std::unordered_set<std::pair<std::string, folly::IPAddress>>>();

    unicastRouteDb->emplace(prefix, newNextHops);
  }
  ++fibSyncCount_;
  syncFibBaton_.post();

  // Identify dirty prefixes that are not part of Unicast route db nor failed
  // Unicast route add. These would be the prefixes that we failed to delete
  for (auto& prefix : *dirtyPrefixes) {
    auto ipPrefix = toIpPrefix(prefix);
    if (unicastRouteDb->count(prefix) or
        std::count(
            failedPrefixesToAdd.begin(), failedPrefixesToAdd.end(), ipPrefix)) {
      continue;
    }
    failedPrefixesToDelete.emplace_back(ipPrefix);
  }

  // Throw FibUpdateError if applicable
  if (failedPrefixesToAdd.size() or failedPrefixesToDelete.size()) {
    thrift::PlatformFibUpdateError error;
    error.vrf2failedAddUpdatePrefixes()->emplace(
        0, std::move(failedPrefixesToAdd));
    error.vrf2failedDeletePrefixes()->emplace(
        0, std::move(failedPrefixesToDelete));
    throw error;
  }
}

void
MockNetlinkFibHandler::addMplsRoutes(
    int16_t, std::unique_ptr<std::vector<openr::thrift::MplsRoute>> routes) {
  ensureHealthy();

  // Acquire locks
  auto mplsRouteDb = mplsRouteDb_.wlock();
  auto dirtyLabels = dirtyLabels_.rlock();

  // Delete routes
  std::vector<int32_t> failedLabels;
  for (auto& route : *routes) {
    // If route is marked dirty add it to exception and continue
    if (dirtyLabels->count(*route.topLabel())) {
      failedLabels.emplace_back(*route.topLabel());
      continue;
    }

    mplsRouteDb->insert_or_assign(
        *route.topLabel(), std::move(*route.nextHops()));
  }
  addMplsRoutesCount_ += routes->size() - failedLabels.size();
  updateMplsRoutesBaton_.post();

  // Throw FibUpdateError if applicable
  if (failedLabels.size()) {
    thrift::PlatformFibUpdateError error;
    error.failedAddUpdateMplsLabels() = std::move(failedLabels);
    throw error;
  }
}

void
MockNetlinkFibHandler::deleteMplsRoutes(
    int16_t, std::unique_ptr<std::vector<int32_t>> labels) {
  ensureHealthy();

  // Acquire locks
  auto mplsRouteDb = mplsRouteDb_.wlock();

  // Delete routes
  for (auto& label : *labels) {
    mplsRouteDb->erase(label);
  }
  delMplsRoutesCount_ += labels->size();
  deleteMplsRoutesBaton_.post();
}

void
MockNetlinkFibHandler::syncMplsFib(
    int16_t, std::unique_ptr<std::vector<openr::thrift::MplsRoute>> routes) {
  ensureHealthy();

  // Acquire locks
  auto mplsRouteDb = mplsRouteDb_.wlock();
  auto dirtyLabels = dirtyLabels_.rlock();

  // Sync routes
  std::vector<int32_t> failedLabelsToAdd, failedLabelsToDelete;
  mplsRouteDb->clear();
  for (auto& route : *routes) {
    // If route is marked dirty add it to exception and continue
    if (dirtyLabels->count(*route.topLabel())) {
      failedLabelsToAdd.emplace_back(*route.topLabel());
      continue;
    }

    mplsRouteDb->emplace(*route.topLabel(), std::move(*route.nextHops()));
  }
  ++fibMplsSyncCount_;
  syncMplsFibBaton_.post();

  // Identify dirty labels that are not part of MPLS route db nor of failed MPLS
  // route add. These would be the labels that we failed to delete
  for (auto& label : *dirtyLabels) {
    if (mplsRouteDb->count(label) or
        std::count(failedLabelsToAdd.begin(), failedLabelsToAdd.end(), label)) {
      continue;
    }
    failedLabelsToDelete.emplace_back(label);
  }

  // Throw FibUpdateError if applicable
  if (failedLabelsToAdd.size() or failedLabelsToDelete.size()) {
    thrift::PlatformFibUpdateError error;
    error.failedAddUpdateMplsLabels() = std::move(failedLabelsToAdd);
    error.failedDeleteMplsLabels() = std::move(failedLabelsToDelete);
    throw error;
  }
}

int64_t
MockNetlinkFibHandler::aliveSince() {
  int64_t res = 0;
  startTime_.withRLock([&](auto& startTime) { res = startTime; });
  return res;
}

void
MockNetlinkFibHandler::getRouteTableByClient(
    std::vector<openr::thrift::UnicastRoute>& routes, int16_t) {
  unicastRouteDb_.withRLock([&](auto& unicastRouteDb) {
    routes.clear();
    VLOG(2) << "MockNetlinkFibHandler: get route table by client";
    for (auto const& [prefix, nhs] : unicastRouteDb) {
      auto thriftNextHops =
          from(nhs) |
          mapped([](const std::pair<std::string, folly::IPAddress>& nextHop) {
            VLOG(2) << "mapping next-hop " << nextHop.second.str() << " dev "
                    << nextHop.first;
            thrift::NextHopThrift thriftNextHop;
            *thriftNextHop.address() = toBinaryAddress(nextHop.second);
            thriftNextHop.address()->ifName() = nextHop.first;
            return thriftNextHop;
          }) |
          as<std::vector>();

      thrift::UnicastRoute route;
      route.dest() = toIpPrefix(prefix);
      route.nextHops() = std::move(thriftNextHops);
      routes.emplace_back(std::move(route));
    }
  });
}

void
MockNetlinkFibHandler::getMplsRouteTableByClient(
    std::vector<openr::thrift::MplsRoute>& routes, int16_t) {
  mplsRouteDb_.withRLock([&](auto& mplsRouteDb) {
    routes.clear();
    for (auto const& [topLabel, nhs] : mplsRouteDb) {
      thrift::MplsRoute route;
      route.topLabel() = topLabel;
      route.nextHops() = nhs;
      routes.emplace_back(std::move(route));
    }
  });
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
MockNetlinkFibHandler::waitForUnhealthyException(size_t count) {
  while (count--) {
    unhealthyExceptionQueue_.get();
  }
}

void
MockNetlinkFibHandler::setDirtyState(
    std::vector<folly::CIDRNetwork> const& dirtyPrefixes,
    std::vector<int32_t> const& dirtyLabels) {
  auto dirtyPrefixesState = dirtyPrefixes_.wlock();
  auto dirtyLabelsState = dirtyLabels_.wlock();
  dirtyPrefixesState->clear();
  dirtyPrefixesState->insert(dirtyPrefixes.begin(), dirtyPrefixes.end());
  dirtyLabelsState->clear();
  dirtyLabelsState->insert(dirtyLabels.begin(), dirtyLabels.end());
}

void
MockNetlinkFibHandler::stop() {
  unicastRouteDb_->clear();
  mplsRouteDb_->clear();
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
  startTime_.withWLock([&](auto& startTime) {
    startTime += 1; // Always increment on restart for unique number
  });
  fibSyncCount_ = 0;
  addRoutesCount_ = 0;
  delRoutesCount_ = 0;
  fibMplsSyncCount_ = 0;
  addMplsRoutesCount_ = 0;
  delMplsRoutesCount_ = 0;
}

void
MockNetlinkFibHandler::ensureHealthy() {
  if (not isHealthy_) {
    unhealthyExceptionQueue_.push(folly::Unit());
    throw std::runtime_error("Handler rejects routes since it is unhealthy");
  }
}

} // namespace openr
