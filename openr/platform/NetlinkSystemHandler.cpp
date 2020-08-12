/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/futures/Promise.h>
#include <glog/logging.h>

#include <openr/common/NetworkUtil.h>
#include <openr/platform/NetlinkSystemHandler.h>

using apache::thrift::FRAGILE;

const std::chrono::seconds kNetlinkDbResyncInterval{20};

namespace openr {

namespace {

std::vector<std::string>
toString(std::vector<thrift::IpPrefix> const& prefixes) {
  std::vector<std::string> strs;
  for (const auto& prefix : prefixes) {
    strs.emplace_back(openr::toString(prefix));
  }
  return strs;
}

} // namespace

NetlinkSystemHandler::NetlinkSystemHandler(fbnl::NetlinkProtocolSocket* nlSock)
    : nlSock_(nlSock) {
  CHECK(nlSock);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::IpPrefix>>>
NetlinkSystemHandler::semifuture_getIfaceAddresses(
    std::unique_ptr<std::string> ifName, int16_t family, int16_t scope) {
  LOG(INFO) << "Querying addresses for interface " << *ifName
            << ", family=" << family << ", scope=" << scope;

  // Get iface index
  const int ifIndex = getIfIndex(*ifName).value();

  return nlSock_->getAllIfAddresses().deferValue(
      [ifIndex, family, scope](
          folly::Expected<std::vector<fbnl::IfAddress>, int>&& nlAddrs) {
        if (nlAddrs.hasError()) {
          throw fbnl::NlException("Failed fetching addrs", nlAddrs.error());
        }
        auto addrs = std::make_unique<std::vector<thrift::IpPrefix>>();
        for (auto& nlAddr : nlAddrs.value()) {
          if (nlAddr.getIfIndex() != ifIndex) {
            continue;
          }
          // Apply filter on family if specified
          if (family && nlAddr.getFamily() != family) {
            continue;
          }
          // Apply filter on scope. Must always be specified
          if (nlAddr.getScope() != scope) {
            continue;
          }
          addrs->emplace_back(toIpPrefix(nlAddr.getPrefix().value()));
        }
        return addrs;
      });
}

folly::SemiFuture<folly::Unit>
NetlinkSystemHandler::semifuture_syncIfaceAddresses(
    std::unique_ptr<std::string> iface,
    int16_t family,
    int16_t scope,
    std::unique_ptr<std::vector<::openr::thrift::IpPrefix>> newAddrs) {
  LOG(INFO) << "Syncing addresses on interface " << *iface
            << ", family=" << family << ", scope=" << scope
            << ", addresses=" << folly::join(",", toString(*newAddrs));

  const auto ifName = *iface; // Copy intended
  const auto ifIndex = getIfIndex(ifName).value();

  auto oldAddrs =
      semifuture_getIfaceAddresses(std::move(iface), family, scope).get();
  std::vector<folly::SemiFuture<int>> futures;

  // Add new addresses
  for (auto& newAddr : *newAddrs) {
    // Skip adding existing addresse
    if (std::find(oldAddrs->begin(), oldAddrs->end(), newAddr) !=
        oldAddrs->end()) {
      continue;
    }
    // Add non-existing new address
    fbnl::IfAddressBuilder builder;
    builder.setPrefix(toIPNetwork(newAddr, false /* applyMask */));
    builder.setIfIndex(ifIndex);
    builder.setScope(scope);
    futures.emplace_back(nlSock_->addIfAddress(builder.build()));
  }

  // Delete old addresses
  for (auto& oldAddr : *oldAddrs) {
    // Skip removing new addresse
    if (std::find(newAddrs->begin(), newAddrs->end(), oldAddr) !=
        newAddrs->end()) {
      continue;
    }
    // Remove non-existing old address
    fbnl::IfAddressBuilder builder;
    builder.setPrefix(toIPNetwork(oldAddr, false /* applyMask */));
    builder.setIfIndex(ifIndex);
    builder.setScope(scope);
    futures.emplace_back(nlSock_->deleteIfAddress(builder.build()));
  }

  // Collect all futures
  return collectAll(std::move(futures))
      .deferValue([](std::vector<folly::Try<int>>&& retvals) {
        for (auto& retval : retvals) {
          const int ret = std::abs(retval.value());
          if (ret != 0 && ret != EEXIST && ret != EADDRNOTAVAIL) {
            throw fbnl::NlException("Address add/remove failed.", ret);
          }
        }
        return folly::Unit();
      });
}

std::optional<int>
NetlinkSystemHandler::getIfIndex(const std::string& ifName) {
  auto links = nlSock_->getAllLinks().get().value();
  for (auto& link : links) {
    if (link.getLinkName() == ifName) {
      return link.getIfIndex();
    }
  }
  return std::nullopt;
}

} // namespace openr
