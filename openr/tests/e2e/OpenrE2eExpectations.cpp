/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrE2eInternal.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <thrift/lib/cpp/transport/TTransportException.h>
#include <thrift/lib/cpp2/async/RpcOptions.h>

#include <openr/common/NetworkUtil.h>

namespace openr::e2e {
namespace {

using Clock = std::chrono::steady_clock;
using NextHop = std::pair<std::string, std::string>;

template <typename Predicate>
bool
awaitUntil(std::chrono::milliseconds timeout, Predicate&& ready) {
  if (timeout.count() < 0) {
    throw std::invalid_argument("Open/R E2E timeout must not be negative");
  }
  const auto deadline = Clock::now() + timeout;
  do {
    if (ready(deadline)) {
      return true;
    }
    std::this_thread::yield();
  } while (Clock::now() < deadline);
  return false;
}

} // namespace

bool
OpenrE2eLab::Impl::hasProgrammedPrefix(const folly::CIDRNetwork& prefix) const {
  for (const auto& route : platform_.fibRoutes()) {
    if (toIPNetwork(*route.dest()) == prefix) {
      return true;
    }
  }
  return false;
}

bool
OpenrE2eLab::Impl::awaitAdjacency(
    const std::string& name, bool isUp, std::chrono::milliseconds timeout) {
  requireStarted();
  const auto& p = peer(name);
  return awaitUntil(timeout, [&](Clock::time_point deadline) {
    apache::thrift::RpcOptions options;
    options.setTimeout(
        std::max(
            std::chrono::milliseconds{1},
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now())));
    std::vector<thrift::SparkNeighbor> neighbors;
    try {
      controlClient_->sync_getNeighbors(options, neighbors);
    } catch (const apache::thrift::transport::TTransportException& error) {
      if (error.getType() ==
          apache::thrift::transport::TTransportException::TIMED_OUT) {
        return false;
      }
      throw;
    }
    const bool established = std::any_of(
        neighbors.begin(), neighbors.end(), [&](const auto& neighbor) {
          return *neighbor.nodeName() == p.name &&
              *neighbor.localIfName() == p.dutIfName &&
              *neighbor.state() == "ESTABLISHED";
        });
    return established == isUp;
  });
}

bool
OpenrE2eLab::Impl::awaitProgrammedRoute(
    const std::string& cidr,
    const std::vector<std::string>& viaPeers,
    std::chrono::milliseconds timeout) {
  requireStarted();
  if (viaPeers.empty()) {
    throw std::invalid_argument("Programmed route requires a next-hop peer");
  }
  std::set<NextHop> expected;
  for (const auto& name : viaPeers) {
    const auto& p = peer(name);
    expected.emplace(
        p.synthetic->dutIfName(),
        folly::IPAddress(p.synthetic->peerV6Address()).str());
  }
  if (expected.size() != viaPeers.size()) {
    throw std::invalid_argument("Duplicate next-hop peer");
  }

  const auto target = folly::IPAddress::createNetwork(cidr);
  return awaitUntil(timeout, [&](Clock::time_point) {
    for (const auto& route : platform_.fibRoutes()) {
      if (toIPNetwork(*route.dest()) != target) {
        continue;
      }
      std::set<NextHop> actual;
      for (const auto& nextHop : *route.nextHops()) {
        actual.emplace(
            nextHop.address()->ifName().value_or(""),
            toIPAddress(*nextHop.address()).str());
      }
      return actual == expected;
    }
    return false;
  });
}

bool
OpenrE2eLab::Impl::awaitRouteAbsent(
    const std::string& cidr, std::chrono::milliseconds timeout) {
  requireStarted();
  const auto target = folly::IPAddress::createNetwork(cidr);
  return awaitUntil(
      timeout, [&](Clock::time_point) { return !hasProgrammedPrefix(target); });
}

bool
OpenrE2eLab::awaitAdjacency(
    const std::string& peer, bool isUp, std::chrono::milliseconds timeout) {
  return impl_->awaitAdjacency(peer, isUp, timeout);
}

bool
OpenrE2eLab::awaitProgrammedRoute(
    const std::string& cidr,
    const std::vector<std::string>& viaPeers,
    std::chrono::milliseconds timeout) {
  return impl_->awaitProgrammedRoute(cidr, viaPeers, timeout);
}

bool
OpenrE2eLab::awaitRouteAbsent(
    const std::string& cidr, std::chrono::milliseconds timeout) {
  return impl_->awaitRouteAbsent(cidr, timeout);
}

} // namespace openr::e2e
