/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PlatformPublisher.h"

#include <fbzmq/zmq/Zmq.h>
#include <glog/logging.h>
#include <thrift/lib/cpp/util/EnumUtils.h>

#include <openr/common/NetworkUtil.h>

namespace openr {

PlatformPublisher::PlatformPublisher(
    fbzmq::Context& context,
    const PlatformPublisherUrl& platformPubUrl,
    fbnl::NetlinkProtocolSocket* nlSock) {
  CHECK_NOTNULL(nlSock);

  // Initialize ZMQ sockets
  platformPubSock_ = fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER>(
      context, folly::none, folly::none, fbzmq::NonblockingFlag{true});
  LOG(INFO) << "Binding PlatformPublisher on "
            << static_cast<std::string>(platformPubUrl);
  const auto platformPub =
      platformPubSock_.bind(fbzmq::SocketUrl{platformPubUrl});
  if (platformPub.hasError()) {
    LOG(FATAL) << "Error binding to URL "
               << static_cast<std::string>(platformPubUrl) << " "
               << platformPub.error();
  }

  // Initialize interface index to name mapping
  for (int i = 0; i < 3; i++) {
    auto nlLinks = nlSock->getAllLinks().get();
    if (nlLinks.hasError() && std::abs(nlLinks.error()) == ETIMEDOUT) {
      CHECK_LT(i, 3) << "Timed out 3 times for fetching links";
      continue;
    }

    CHECK(nlLinks.hasValue())
        << fbnl::NlException("Error getting links", nlLinks.error()).what();
    for (auto& link : nlLinks.value()) {
      ifIndexToName_.emplace(link.getIfIndex(), link.getLinkName());
    }
  }

  // Attach callbacks for link events
  nlSock->setLinkEventCB([this](fbnl::Link link, bool /* ignore */) {
    // Cache interface index name mapping
    ifIndexToName_[link.getIfIndex()] = link.getLinkName();

    thrift::LinkEntry event;
    event.ifIndex = link.getIfIndex();
    event.ifName = link.getLinkName();
    event.isUp = link.isUp();
    event.weight = Constants::kDefaultAdjWeight;
    LOG(INFO) << "Link Event: " << link.str();
    publishLinkEvent(event);
  });

  // Attach callbacks for address events
  nlSock->setAddrEventCB([this](fbnl::IfAddress addr, bool /* ignore */) {
    // Check for interface name
    auto it = ifIndexToName_.find(addr.getIfIndex());
    if (it == ifIndexToName_.end()) {
      LOG(ERROR) << "Address event for unknown interface " << addr.str();
      return;
    }

    // Check for valid prefix
    if (not addr.getPrefix().has_value()) {
      LOG(ERROR) << "Address event with empty address " << addr.str();
      return;
    }

    thrift::AddrEntry event;
    event.ifName = it->second;
    event.ipPrefix = toIpPrefix(addr.getPrefix().value());
    event.isValid = addr.isValid();
    LOG(INFO) << "Address Event: " << addr.str();
    publishAddrEvent(event);
  });
}

void
PlatformPublisher::publishLinkEvent(const thrift::LinkEntry& link) {
  // advertise change of link, prompting subscriber modules to
  // take immediate action
  thrift::PlatformEvent msg;
  msg.eventType = thrift::PlatformEventType::LINK_EVENT;
  msg.eventData = fbzmq::util::writeThriftObjStr(link, serializer_);
  publishPlatformEvent(msg);
}

void
PlatformPublisher::publishAddrEvent(const thrift::AddrEntry& address) {
  // advertise change of address, prompting subscriber modules to
  // take immediate action
  thrift::PlatformEvent msg;
  msg.eventType = thrift::PlatformEventType::ADDRESS_EVENT;
  msg.eventData = fbzmq::util::writeThriftObjStr(address, serializer_);
  publishPlatformEvent(msg);
}

void
PlatformPublisher::publishPlatformEvent(const thrift::PlatformEvent& msg) {
  thrift::PlatformEventType eventType = msg.eventType;
  // send header of event in the first 2 byte
  platformPubSock_.sendMore(
      fbzmq::Message::from(static_cast<uint16_t>(eventType)).value());
  const auto sendNeighEntry = platformPubSock_.sendThriftObj(msg, serializer_);
  if (sendNeighEntry.hasError()) {
    LOG(ERROR) << "Error in sending PlatformEventType Entry, event Type: "
               << apache::thrift::util::enumNameSafe(msg.eventType);
  }
}

void
PlatformPublisher::stop() {
  platformPubSock_.close();
}

} // namespace openr
