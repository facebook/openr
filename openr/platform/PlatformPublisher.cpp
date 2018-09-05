/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PlatformPublisher.h"

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/MapUtil.h>
#include <folly/gen/Base.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AddressUtil.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>

using apache::thrift::CompactSerializer;
using apache::thrift::FRAGILE;

namespace openr {

PlatformPublisher::PlatformPublisher(
    fbzmq::Context& context, const PlatformPublisherUrl& platformPubUrl)
    : platformPubUrl_(platformPubUrl) {
  // Initialize ZMQ sockets
  platformPubSock_ = fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER>(
      context, folly::none, folly::none, fbzmq::NonblockingFlag{true});
  VLOG(2) << "Platform Publisher: Binding pub url '" << platformPubUrl_ << "'";
  const auto platformPub =
      platformPubSock_.bind(fbzmq::SocketUrl{platformPubUrl_});
  if (platformPub.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << platformPubUrl_ << "' "
               << platformPub.error();
  }
}

void
PlatformPublisher::publishLinkEvent(const thrift::LinkEntry& link) const {
  // advertise change of link, prompting subscriber modules to
  // take immediate action
  thrift::PlatformEvent msg;
  msg.eventType = thrift::PlatformEventType::LINK_EVENT;
  msg.eventData = fbzmq::util::writeThriftObjStr(link, serializer_);
  publishPlatformEvent(msg);
}

void
PlatformPublisher::publishAddrEvent(const thrift::AddrEntry& address) const {
  // advertise change of address, prompting subscriber modules to
  // take immediate action
  thrift::PlatformEvent msg;
  msg.eventType = thrift::PlatformEventType::ADDRESS_EVENT;
  msg.eventData = fbzmq::util::writeThriftObjStr(address, serializer_);
  publishPlatformEvent(msg);
}

void
PlatformPublisher::publishNeighborEvent(
    const thrift::NeighborEntry& neighbor) const {
  // advertise change of neighbor, prompting subscriber modules to
  // take immediate action
  thrift::PlatformEvent msg;
  msg.eventType = thrift::PlatformEventType::NEIGHBOR_EVENT;
  msg.eventData = fbzmq::util::writeThriftObjStr(neighbor, serializer_);
  publishPlatformEvent(msg);
}

void
PlatformPublisher::publishPlatformEvent(
    const thrift::PlatformEvent& msg) const {
  VLOG(3) << "Publishing PlatformEvent...";
  thrift::PlatformEventType eventType = msg.eventType;
  // send header of event in the first 2 byte
  platformPubSock_.sendMore(
      fbzmq::Message::from(static_cast<uint16_t>(eventType)).value());
  const auto sendNeighEntry = platformPubSock_.sendThriftObj(msg, serializer_);
  if (sendNeighEntry.hasError()) {
    LOG(ERROR) << "Error in sending PlatformEventType Entry, event Type: "
               << folly::get_default(
                      thrift::_PlatformEventType_VALUES_TO_NAMES,
                      msg.eventType,
                      "UNKNOWN");
  }
}

void
PlatformPublisher::linkEventFunc(
    const std::string& ifName,
    const openr::fbnl::Link& linkEntry) {
  VLOG(4) << "Handling Link Event in NetlinkSystemHandler...";
  publishLinkEvent(thrift::LinkEntry(
      FRAGILE,
      ifName,
      linkEntry.getIfIndex(),
      linkEntry.isUp(),
      Constants::kDefaultAdjWeight));
}

void
PlatformPublisher::addrEventFunc(
    const std::string& ifName,
    const openr::fbnl::IfAddress& addrEntry) {
  VLOG(4) << "Handling Address Event in NetlinkSystemHandler...";
  publishAddrEvent(thrift::AddrEntry(
      FRAGILE,
      ifName,
      toIpPrefix(addrEntry.getPrefix().value()),
      addrEntry.isValid()));
}

void
PlatformPublisher::neighborEventFunc(
    const std::string& ifName,
    const openr::fbnl::Neighbor& neighborEntry) {
  VLOG(4) << "Handling Neighbor Event in NetlinkSystemHandler...";
  publishNeighborEvent(thrift::NeighborEntry(
      FRAGILE,
      ifName,
      toBinaryAddress(neighborEntry.getDestination()),
      neighborEntry.getLinkAddress().value().toString(),
      neighborEntry.isReachable()));
}

void
PlatformPublisher::stop() {
  platformPubSock_.close();
}

} // namespace openr
