/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrE2eInternal.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <thrift/lib/cpp/TApplicationException.h>
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
    const auto now = Clock::now();
    if (now < deadline) {
      // Snapshot-only external APIs require paced, deadline-bounded probes.

      // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_until)
      std::this_thread::sleep_until(
          std::min(deadline, now + std::chrono::milliseconds{5}));
    }
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
      using TransportError = apache::thrift::transport::TTransportException;
      const auto type = error.getType();
      if (type != TransportError::TIMED_OUT &&
          type != TransportError::NOT_OPEN &&
          type != TransportError::END_OF_FILE &&
          type != TransportError::NETWORK_ERROR &&
          (type != TransportError::INTERNAL_ERROR ||
           error.getErrno() != ECONNRESET)) {
        throw;
      }
      reconnectControlClient();
      return false;
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
OpenrE2eLab::Impl::awaitPeerPrefix(
    const std::string& name,
    const std::string& cidr,
    bool isAdvertised,
    std::chrono::milliseconds timeout) {
  requireStarted();
  const auto& p = peer(name);
  const auto prefix = folly::IPAddress::createNetwork(cidr);
  return awaitUntil(timeout, [&](Clock::time_point) {
    return p.synthetic->receivedPrefix(nodeName_, prefix, isAdvertised);
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
OpenrE2eLab::Impl::awaitComputedRoute(
    const std::string& cidr, std::chrono::milliseconds timeout) {
  requireStarted();
  const auto target = folly::IPAddress::createNetwork(cidr);
  return awaitUntil(timeout, [&](Clock::time_point deadline) {
    apache::thrift::RpcOptions options;
    options.setTimeout(
        std::max(
            std::chrono::milliseconds{1},
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now())));
    thrift::RouteDatabase routes;
    try {
      controlClient_->sync_getRouteDbComputed(options, routes, "");
    } catch (const apache::thrift::transport::TTransportException& error) {
      if (error.getType() !=
          apache::thrift::transport::TTransportException::TIMED_OUT) {
        throw;
      }
      return false;
    } catch (const apache::thrift::TApplicationException& error) {
      if (error.getType() != apache::thrift::TApplicationException::TIMEOUT) {
        throw;
      }
      return false;
    }
    return std::any_of(
        routes.unicastRoutes()->begin(),
        routes.unicastRoutes()->end(),
        [&](const auto& route) {
          return toIPNetwork(*route.dest()) == target;
        });
  });
}

bool
OpenrE2eLab::awaitAdjacency(
    const std::string& peer, bool isUp, std::chrono::milliseconds timeout) {
  return impl_->awaitAdjacency(peer, isUp, timeout);
}

bool
OpenrE2eLab::awaitPeerPrefix(
    const std::string& peer,
    const std::string& cidr,
    bool isAdvertised,
    std::chrono::milliseconds timeout) {
  return impl_->awaitPeerPrefix(peer, cidr, isAdvertised, timeout);
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

bool
OpenrE2eLab::awaitComputedRoute(
    const std::string& cidr, std::chrono::milliseconds timeout) {
  return impl_->awaitComputedRoute(cidr, timeout);
}

} // namespace openr::e2e
