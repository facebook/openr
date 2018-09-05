/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetlinkSystemHandler.h"

#include <algorithm>
#include <functional>
#include <thread>
#include <utility>

#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/futures/Promise.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/transport/THeader.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/protocol/BinaryProtocol.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/AddressUtil.h>

using namespace fbzmq;
using apache::thrift::FRAGILE;

const std::chrono::seconds kNetlinkDbResyncInterval{20};

namespace openr {
NetlinkSystemHandler::NetlinkSystemHandler(
    fbzmq::ZmqEventLoop* zmqEventLoop,
    std::shared_ptr<fbnl::NetlinkSocket> netlinkSocket)
    : mainEventLoop_(zmqEventLoop), netlinkSocket_(netlinkSocket) {}

NetlinkSystemHandler::~NetlinkSystemHandler() {}


folly::Future<std::unique_ptr<std::vector<thrift::Link>>>
NetlinkSystemHandler::future_getAllLinks() {
  VLOG(3) << "Query links from Netlink according to link name";

  folly::Promise<std::unique_ptr<std::vector<thrift::Link>>> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise)] () mutable {
       try {
         auto links = doGetAllLinks();
         p.setValue(std::move(links));
       } catch (const std::exception& ex) {
         p.setException(ex);
       }
     });
  return future;
}

std::unique_ptr<std::vector<thrift::Link>>
NetlinkSystemHandler::doGetAllLinks() {

  auto linkDb = std::make_unique<std::vector<thrift::Link>>();
  auto links = netlinkSocket_->getAllLinks().get();

  linkDb->clear();

  for (const auto kv : links) {
    thrift::Link linkEntry;
    linkEntry.ifName = kv.first;
    linkEntry.ifIndex = kv.second.ifIndex;
    linkEntry.isUp = kv.second.isUp;
    for (const auto network : kv.second.networks) {
      linkEntry.networks.push_back(thrift::IpPrefix(
          FRAGILE, toBinaryAddress(network.first), network.second));
    }
    linkDb->push_back(linkEntry);
  }
  return linkDb;
}

folly::Future<std::unique_ptr<std::vector<thrift::NeighborEntry>>>
NetlinkSystemHandler::future_getAllNeighbors() {
  VLOG(3) << "Query all reachable neighbors from Netlink";

  folly::Promise<std::unique_ptr<std::vector<thrift::NeighborEntry>>> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise)] () mutable {
       try {
         auto links = doGetAllNeighbors();
         p.setValue(std::move(links));
       } catch (const std::exception& ex) {
         p.setException(ex);
       }
     });
  return future;
}

std::unique_ptr<std::vector<openr::thrift::NeighborEntry>>
NetlinkSystemHandler::doGetAllNeighbors() {
  auto neighborDb = std::make_unique<std::vector<thrift::NeighborEntry>>();
  const auto& neighbors = netlinkSocket_->getAllReachableNeighbors().get();

  neighborDb->clear();

  for (const auto& kv : neighbors) {
    thrift::NeighborEntry neighborEntry = thrift::NeighborEntry(
        FRAGILE,
        kv.first.first,
        toBinaryAddress(kv.first.second),
        kv.second.getLinkAddress().value().toString(),
        true);
    neighborDb->push_back(neighborEntry);
  }
  return neighborDb;
}

folly::Future<folly::Unit> NetlinkSystemHandler::future_addIfaceAddresses(
  std::unique_ptr<std::string> iface,
  std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Add iface addresses";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     addresses = std::move(addrs),
     ifName = std::move(iface)]() mutable {
      try {
        for (const auto& addr : *addresses) {
          const auto& prefix = toIPNetwork(addr);
          doAddIfaceAddr(*ifName, prefix);
        }
        p.setValue();
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
    return future;
}

void NetlinkSystemHandler::doAddIfaceAddr(
  const std::string& ifName,
  const folly::CIDRNetwork& prefix) {
  int ifIndex = netlinkSocket_->getIfIndex(ifName).get();
  fbnl::IfAddressBuilder builder;
  auto addr = builder.setPrefix(prefix)
                     .setIfIndex(ifIndex)
                     .build();
  netlinkSocket_->addIfAddress(std::move(addr)).get();
}

folly::Future<folly::Unit> NetlinkSystemHandler::future_removeIfaceAddresses(
  std::unique_ptr<std::string> iface,
  std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Remove iface addresses";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     addresses = std::move(addrs),
     ifName = std::move(iface)]() mutable {
      try {
        for (const auto& addr : *addresses) {
          const auto& prefix = toIPNetwork(addr);
          doRemoveIfaceAddr(*ifName, prefix);
        }
        p.setValue();
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
    return future;
}

folly::Future<std::unique_ptr<std::vector<::openr::thrift::IpPrefix>>>
NetlinkSystemHandler::future_getIfaceAddresses(
  std::unique_ptr<std::string> iface, int16_t family, int16_t scope) {
  VLOG(3) << "Get iface addresses";
  folly::Promise<std::unique_ptr<
                  std::vector<::openr::thrift::IpPrefix>>> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     ifName = std::move(iface), family, scope] () mutable {
       try {
         auto addrs = doGetIfaceAddrs(*ifName, family, scope);
         p.setValue(std::move(addrs));
       } catch (const std::exception& ex) {
         p.setException(ex);
       }
     });
  return future;
}

std::unique_ptr<std::vector<openr::thrift::IpPrefix>>
NetlinkSystemHandler::doGetIfaceAddrs(
  const std::string& iface,
  int16_t family,
  int16_t scope) {
    auto addrs = std::make_unique<std::vector<openr::thrift::IpPrefix>>();
    int ifIndex = netlinkSocket_->getIfIndex(iface).get();
    auto ifAddrs =
      netlinkSocket_->getIfAddrs(ifIndex, family, scope).get();

    addrs->clear();
    for (const auto& ifAddr : ifAddrs) {
      addrs->emplace_back(toIpPrefix(ifAddr.getPrefix().value()));
    }
    return addrs;
}


void NetlinkSystemHandler::doRemoveIfaceAddr(
  const std::string& ifName,
  const folly::CIDRNetwork& prefix) {
  int ifIndex = netlinkSocket_->getIfIndex(ifName).get();
  fbnl::IfAddressBuilder builder;
  builder.setPrefix(prefix)
         .setIfIndex(ifIndex);
  netlinkSocket_->delIfAddress(builder.build()).get();
}

folly::Future<folly::Unit> NetlinkSystemHandler::future_syncIfaceAddresses(
  std::unique_ptr<std::string> iface,
  int16_t family, int16_t scope,
  std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> addrs) {
  VLOG(3) << "Sync iface addresses";
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  mainEventLoop_->runInEventLoop(
    [this, p = std::move(promise),
     ifName = std::move(iface),
     addresses = std::move(addrs),
     family, scope]() mutable {
      try {
        doSyncIfaceAddrs(*ifName, family, scope, *addresses);
        p.setValue();
      } catch (const std::exception& ex) {
        p.setException(ex);
      }
    });
    return future;
}

void NetlinkSystemHandler::doSyncIfaceAddrs(
  const std::string& ifName,
  int16_t family,
  int16_t scope,
  const std::vector<::openr::thrift::IpPrefix>& addrs) {
  int ifIndex = netlinkSocket_->getIfIndex(ifName).get();
  std::vector<fbnl::IfAddress> ifAddrs;
  fbnl::IfAddressBuilder builder;
  for (const auto& addr : addrs) {
    builder.setFamily(family)
           .setIfIndex(ifIndex)
           .setScope(scope)
           .setPrefix(toIPNetwork(addr));
    ifAddrs.emplace_back(builder.build());
    builder.reset();
  }
  netlinkSocket_->
    syncIfAddress(ifIndex, std::move(ifAddrs), family, scope).get();
}

} // namespace openr
