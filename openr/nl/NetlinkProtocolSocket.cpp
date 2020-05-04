/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <thread>

#include <fb303/ServiceData.h>

#include <openr/common/Util.h>
#include <openr/nl/NetlinkProtocolSocket.h>

namespace fb303 = facebook::fb303;

namespace openr::fbnl {

NetlinkProtocolSocket::NetlinkProtocolSocket(
    fbzmq::ZmqEventLoop* evl, bool enableIPv6RouteReplaceSemantics)
    : evl_(evl),
      enableIPv6RouteReplaceSemantics_(enableIPv6RouteReplaceSemantics) {
  nlMessageTimer_ = fbzmq::ZmqTimeout::make(evl_, [this]() noexcept {
    DCHECK(false) << "This shouldn't occur usually. Adding DCHECK to get "
                  << "attention in UTs";

    fb303::fbData->addStatValue(
        "netlink.message_timeouts", nlSeqNumMap_.size(), fb303::SUM);

    LOG(ERROR) << "Timed-out receiving ack for " << nlSeqNumMap_.size()
               << " message(s).";
    for (auto const& kv : nlSeqNumMap_) {
      LOG(ERROR) << "  Pending seq=" << kv.first << ", message-type="
                 << static_cast<int>(kv.second->getMessageType())
                 << ", bytes-sent=" << kv.second->getDataLength();
    }

    LOG(INFO) << "Closing netlink socket and recreate it";
    nlSeqNumMap_.clear(); // Clear all timed out requests
    evl_->removeSocketFd(nlSock_);
    close(nlSock_);
    init();

    LOG(INFO) << "Resume sending bufferred netlink messages";
    sendNetlinkMessage();
  });

  // Initialize the socket in an event loop
  evl_->runInEventLoop([this]() { init(); });
}

void
NetlinkProtocolSocket::init() {
  // Create netlink socket
  nlSock_ = ::socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (nlSock_ < 0) {
    LOG(FATAL) << "Netlink socket create failed.";
  }
  VLOG(1) << "Created netlink socket. fd=" << nlSock_;
  int size = kNetlinkSockRecvBuf;
  // increase socket recv buffer size
  if (setsockopt(nlSock_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
    LOG(FATAL) << "Netlink socket set recv buffer failed.";
  };

  // Bind on the source address. We let kernel chose the available port-ID
  struct sockaddr_nl saddr;
  ::memset(&saddr, 0, sizeof(saddr));
  saddr.nl_family = AF_NETLINK;
  saddr.nl_pid = 0; // We let kernel assign the port-ID
  /* We can subscribe to different Netlink mutlicast groups for specific types
   * of events: link, IPv4/IPv6 address and neighbor. */
  saddr.nl_groups = RTMGRP_LINK // listen for link events
      | RTMGRP_IPV4_IFADDR // listen for IPv4 address events
      | RTMGRP_IPV6_IFADDR // listen for IPv6 address events
      | RTMGRP_NEIGH; // listen for Neighbor (ARP) events

  if (bind(nlSock_, (struct sockaddr*)&saddr, sizeof(saddr)) != 0) {
    LOG(FATAL) << "Failed to bind netlink socket: " << folly::errnoStr(errno);
  }

  // Retrieve and set pid that we will use for all subsequent messages
  portId_ = saddr.nl_pid;

  evl_->addSocketFd(nlSock_, ZMQ_POLLIN, [this](int) noexcept {
    try {
      recvNetlinkMessage();
    } catch (std::exception const& err) {
      LOG(ERROR) << "error processing NL message" << folly::exceptionStr(err);
      fb303::fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    }
  });
}

void
NetlinkProtocolSocket::setLinkEventCB(
    std::function<void(fbnl::Link, bool)> linkEventCB) {
  linkEventCB_ = linkEventCB;
}

void
NetlinkProtocolSocket::setAddrEventCB(
    std::function<void(fbnl::IfAddress, bool)> addrEventCB) {
  addrEventCB_ = addrEventCB;
}

void
NetlinkProtocolSocket::setNeighborEventCB(
    std::function<void(fbnl::Neighbor, bool)> neighborEventCB) {
  neighborEventCB_ = neighborEventCB;
}

void
NetlinkProtocolSocket::processAck(uint32_t ack, int status) {
  auto it = nlSeqNumMap_.find(ack);
  if (it != nlSeqNumMap_.end()) {
    VLOG(2) << "Setting return value for seq=" << ack << " with ret=" << status;
    // Set return status on promise
    it->second->setReturnStatus(status);
    nlSeqNumMap_.erase(it);
  } else {
    LOG(ERROR) << "No future associated with seq=" << ack;
  }

  // Cancel timer if there are no more expected responses
  if (nlSeqNumMap_.empty()) {
    nlMessageTimer_->cancelTimeout();
  } else {
    // Extend timer and wait for next ack
    nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
  }

  // We've successfully completed at-least one message. Send more messages
  // if any pending. Here we add optimization to wait for some more acks and
  // send pending message in batch of atleast `kMinIovMsg`
  if (nlSeqNumMap_.empty() or (kMaxIovMsg - nlSeqNumMap_.size() > kMinIovMsg)) {
    sendNetlinkMessage();
  }
}

void
NetlinkProtocolSocket::sendNetlinkMessage() {
  CHECK(evl_->isInEventLoop());
  struct sockaddr_nl nladdr = {
      .nl_family = AF_NETLINK, .nl_pad = 0, .nl_pid = 0, .nl_groups = 0};
  CHECK_LE(nlSeqNumMap_.size(), kMaxIovMsg)
      << "We must have capacity to send at-least one message!";
  uint32_t count{0};
  const uint32_t iovSize =
      std::min(msgQueue_.size(), kMaxIovMsg - nlSeqNumMap_.size());

  if (!iovSize) {
    return;
  }

  auto iov = std::make_unique<struct iovec[]>(iovSize);

  while (count < iovSize && !msgQueue_.empty()) {
    auto m = std::move(msgQueue_.front());
    msgQueue_.pop();

    struct nlmsghdr* nlmsg_hdr = m->getMessagePtr();
    iov[count].iov_base = reinterpret_cast<void*>(m->getMessagePtr());
    iov[count].iov_len = m->getDataLength();

    // fill sequence number and PID
    nlmsg_hdr->nlmsg_pid = portId_;
    nlmsg_hdr->nlmsg_seq = nextNlSeqNum_++;
    if (nextNlSeqNum_ == 0) {
      // wrap around - we start from 1
      nextNlSeqNum_ = 1;
    }

    // check if one request per message
    if ((nlmsg_hdr->nlmsg_flags & NLM_F_MULTI) != 0) {
      LOG(ERROR) << "Error: multipart netlink message not supported";
    }

    // Add seq number -> netlink request mapping
    auto res = nlSeqNumMap_.insert({nlmsg_hdr->nlmsg_seq, std::move(m)});
    CHECK(res.second) << "Entry exists for " << nlmsg_hdr->nlmsg_seq;
    count++;
  }

  auto outMsg = std::make_unique<struct msghdr>();
  outMsg->msg_name = &nladdr;
  outMsg->msg_namelen = sizeof(nladdr);
  outMsg->msg_iov = &iov[0];
  outMsg->msg_iovlen = count;

  VLOG(2) << "Sending " << outMsg->msg_iovlen << " netlink messages";
  auto status = sendmsg(nlSock_, outMsg.get(), 0);
  if (status < 0) {
    LOG(ERROR) << "Error sending on NL socket "
               << folly::errnoStr(std::abs(status))
               << " Number of messages:" << outMsg->msg_iovlen;
    fb303::fbData->addStatValue("netlink.errors", 1, fb303::SUM);
  }

  // Schedule timer to wait for acks and send next set of messages
  nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
}

void
NetlinkProtocolSocket::processMessage(
    const std::array<char, kMaxNlPayloadSize>& rxMsg, uint32_t bytesRead) {
  // first netlink message header
  struct nlmsghdr* nlh = (struct nlmsghdr*)rxMsg.data();
  do {
    if (!NLMSG_OK(nlh, bytesRead)) {
      break;
    }

    VLOG(2) << "Received Netlink message of type " << nlh->nlmsg_type
            << " seq no " << nlh->nlmsg_seq;
    auto nlSeqIt = nlSeqNumMap_.find(nlh->nlmsg_seq);

    switch (nlh->nlmsg_type) {
    case RTM_NEWROUTE:
    case RTM_DELROUTE: {
      // next RTM message to be processed
      auto route = NetlinkRouteMessage::parseMessage(nlh);
      if (nlSeqIt != nlSeqNumMap_.end()) {
        // Extend message timer as we received a valid ack
        nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
        // Received route in response to request
        nlSeqIt->second->rcvdRoute(std::move(route));
      } else {
        // Route notification
        DCHECK(false) << "Route notifications are not subscribed";
      }
    } break;

    case RTM_DELLINK:
    case RTM_NEWLINK: {
      // process link information received from netlink
      auto link = NetlinkLinkMessage::parseMessage(nlh);

      if (nlSeqIt != nlSeqNumMap_.end()) {
        // Extend message timer as we received a valid ack
        nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
        // Received link in response to request
        nlSeqIt->second->rcvdLink(std::move(link));
      } else {
        // Link notification
        if (linkEventCB_) {
          linkEventCB_(std::move(link), true);
        }
      }
    } break;

    case RTM_DELADDR:
    case RTM_NEWADDR: {
      // process interface address information received from netlink
      auto addr = NetlinkAddrMessage::parseMessage(nlh);
      if (!addr.getPrefix().has_value()) {
        break;
      }

      if (nlSeqIt != nlSeqNumMap_.end()) {
        // Extend message timer as we received a valid ack
        nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
        // Response to a corresponding request
        auto& request = nlSeqIt->second;
        if (request->getMessageType() ==
            NetlinkMessage::MessageType::GET_ALL_ADDRS) {
          // Received link in response to request
          request->rcvdIfAddress(std::move(addr));
        } else {
          // TODO: Make the notifications consistent across all the APIs. We
          // can also invoke notifcation on success of addition or removal.
          //
          // Response to a add/del request - generate addr event for handler.
          // This occurs when we add/del IPv4 addresses generates address event
          // with the same sequence as the original request.
          //
          // IfAddress notification
          if (addrEventCB_) {
            addrEventCB_(std::move(addr), true);
          }
        }
      } else {
        // IfAddress notification
        if (addrEventCB_) {
          addrEventCB_(std::move(addr), true);
        }
      }
    } break;

    case RTM_DELNEIGH:
    case RTM_NEWNEIGH: {
      // process neighbor information received from netlink
      auto neighbor = NetlinkNeighborMessage::parseMessage(nlh);

      if (nlSeqIt != nlSeqNumMap_.end()) {
        // Extend message timer as we received a valid ack
        nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
        // Received neighbor in response to request
        nlSeqIt->second->rcvdNeighbor(std::move(neighbor));
      } else {
        // Neighbor notification
        if (neighborEventCB_) {
          neighborEventCB_(std::move(neighbor), true);
        }
      }
    } break;

    case NLMSG_ERROR: {
      const struct nlmsgerr* const ack =
          reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(nlh));
      if (ack->msg.nlmsg_pid != portId_) {
        LOG(ERROR) << "received netlink message with wrong PID, received: "
                   << ack->msg.nlmsg_pid << " expected: " << portId_;
        break;
      }
      if (std::abs(ack->error) != EEXIST && std::abs(ack->error) != ESRCH &&
          ack->error != 0) {
        fb303::fbData->addStatValue("netlink.errors", 1, fb303::SUM);
      } else {
        fb303::fbData->addStatValue("netlink.acks", 1, fb303::SUM);
      }
      processAck(ack->msg.nlmsg_seq, ack->error);
    } break;

    case NLMSG_NOOP:
      break;

    case NLMSG_DONE: {
      // End of multipart message
      processAck(nlh->nlmsg_seq, 0);
    } break;

    default:
      LOG(ERROR) << "Unknown message type: " << nlh->nlmsg_type;
      fb303::fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    }
  } while ((nlh = NLMSG_NEXT(nlh, bytesRead)));
}

void
NetlinkProtocolSocket::recvNetlinkMessage() {
  // messages buffer
  std::array<char, kMaxNlPayloadSize> recvMsg = {};

  int32_t bytesRead = ::recv(nlSock_, recvMsg.data(), kMaxNlPayloadSize, 0);
  VLOG(4) << "Message received with size: " << bytesRead;

  if (bytesRead < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return;
    }
    LOG(INFO) << "Error in netlink socket receive: " << bytesRead
              << " err: " << folly::errnoStr(std::abs(errno));
    return;
  }
  processMessage(recvMsg, static_cast<uint32_t>(bytesRead));
}

NetlinkProtocolSocket::~NetlinkProtocolSocket() {
  LOG(INFO) << "Closing netlink socket.";
  close(nlSock_);
}

void
NetlinkProtocolSocket::addNetlinkMessage(
    std::unique_ptr<NetlinkMessage> nlmsg) {
  evl_->runImmediatelyOrInEventLoop([this, nlmsg = std::move(nlmsg)]() mutable {
    msgQueue_.push(std::move(nlmsg));
    // call send messages API if no timers are scheduled
    if (!nlMessageTimer_->isScheduled()) {
      sendNetlinkMessage();
    }
  });
}

folly::SemiFuture<int>
NetlinkProtocolSocket::collectReturnStatus(
    std::vector<folly::SemiFuture<int>>&& futures,
    std::unordered_set<int> ignoredErrors) {
  return folly::collectAll(std::move(futures))
      .defer([ignoredErrors](
                 folly::Try<std::vector<folly::Try<int>>>&& results) {
        for (auto& result : results.value()) {
          auto retval = std::abs(result.value()); // Throws exeption if any
          if (retval == 0 or ignoredErrors.count(retval)) {
            continue;
          }

          // We encountered first non zero value. Report error and return
          LOG(ERROR) << "One or more Netlink requests failed with error code: "
                     << retval << " -- " << folly::errnoStr(retval);
          return retval;
        }
        return 0;
      });
}

folly::SemiFuture<int>
NetlinkProtocolSocket::addRoute(const openr::fbnl::Route& route) {
  auto rtmMsg = std::make_unique<NetlinkRouteMessage>();
  auto future = rtmMsg->getSemiFuture();

  int status{0};
  switch (route.getFamily()) {
  case AF_INET6:
    if (not enableIPv6RouteReplaceSemantics_) {
      // Special case for IPv6 route add. We first delete the route and then
      // add it.
      // NOTE: We ignore the error for the deleteRoute
      deleteRoute(route);
    }
    FOLLY_FALLTHROUGH;
  case AF_INET:
    status = rtmMsg->addRoute(route);
    break;
  case AF_MPLS:
    status = rtmMsg->addLabelRoute(route);
    break;
  default:
    status = -EPROTONOSUPPORT;
  }

  if (status != 0) {
    rtmMsg->setReturnStatus(status);
  } else {
    addNetlinkMessage(std::move(rtmMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::deleteRoute(const openr::fbnl::Route& route) {
  auto rtmMsg = std::make_unique<openr::fbnl::NetlinkRouteMessage>();
  auto future = rtmMsg->getSemiFuture();

  int status{0};
  switch (route.getFamily()) {
  case AF_INET:
  case AF_INET6:
    status = rtmMsg->deleteRoute(route);
    break;
  case AF_MPLS:
    status = rtmMsg->deleteLabelRoute(route);
    break;
  default:
    status = -EPROTONOSUPPORT;
  }

  if (status != 0) {
    rtmMsg->setReturnStatus(status);
  } else {
    addNetlinkMessage(std::move(rtmMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::addIfAddress(const openr::fbnl::IfAddress& ifAddr) {
  auto addrMsg = std::make_unique<openr::fbnl::NetlinkAddrMessage>();
  auto future = addrMsg->getSemiFuture();

  // Initialize Netlink message fields to add interface address
  addrMsg->setMessageType(NetlinkMessage::MessageType::ADD_ADDR);
  int status = addrMsg->addOrDeleteIfAddress(ifAddr, RTM_NEWADDR);
  if (status != 0) {
    addrMsg->setReturnStatus(status);
  } else {
    addNetlinkMessage(std::move(addrMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::deleteIfAddress(const openr::fbnl::IfAddress& ifAddr) {
  auto addrMsg = std::make_unique<openr::fbnl::NetlinkAddrMessage>();
  auto future = addrMsg->getSemiFuture();

  // Initialize Netlink message fields to delete interface address
  addrMsg->setMessageType(NetlinkMessage::MessageType::DEL_ADDR);
  int status = addrMsg->addOrDeleteIfAddress(ifAddr, RTM_DELADDR);
  if (status != 0) {
    addrMsg->setReturnStatus(status);
  } else {
    addNetlinkMessage(std::move(addrMsg));
  }

  return future;
}

folly::SemiFuture<std::vector<fbnl::Link>>
NetlinkProtocolSocket::getAllLinks() {
  auto linkMsg = std::make_unique<openr::fbnl::NetlinkLinkMessage>();
  auto future = linkMsg->getLinksSemiFuture();

  // Initialize message fields to get all links
  linkMsg->init(RTM_GETLINK, 0);
  addNetlinkMessage(std::move(linkMsg));

  return future;
}

folly::SemiFuture<std::vector<fbnl::IfAddress>>
NetlinkProtocolSocket::getAllIfAddresses() {
  auto addrMsg = std::make_unique<openr::fbnl::NetlinkAddrMessage>();
  auto future = addrMsg->getAddrsSemiFuture();

  // Initialize message fields to get all addresses
  addrMsg->init(RTM_GETADDR);
  addrMsg->setMessageType(NetlinkMessage::MessageType::GET_ALL_ADDRS);
  addNetlinkMessage(std::move(addrMsg));

  return future;
}

folly::SemiFuture<std::vector<fbnl::Neighbor>>
NetlinkProtocolSocket::getAllNeighbors() {
  auto neighMsg = std::make_unique<openr::fbnl::NetlinkNeighborMessage>();
  auto future = neighMsg->getNeighborsSemiFuture();

  // Initialize message fields to get all neighbors
  neighMsg->init(RTM_GETNEIGH, 0);
  addNetlinkMessage(std::move(neighMsg));

  return future;
}

folly::SemiFuture<std::vector<fbnl::Route>>
NetlinkProtocolSocket::getRoutes(const fbnl::Route& filter) {
  auto routeMsg = std::make_unique<openr::fbnl::NetlinkRouteMessage>();
  auto future = routeMsg->getRoutesSemiFuture();

  // Initialize message fields to get all addresses
  routeMsg->init(RTM_GETROUTE, 0, filter);
  addNetlinkMessage(std::move(routeMsg));

  return future;
}

folly::SemiFuture<std::vector<fbnl::Route>>
NetlinkProtocolSocket::getAllRoutes() {
  fbnl::RouteBuilder builder;
  builder.setProtocolId(RTPROT_UNSPEC); // Explicitly set protocol to 0
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  return getRoutes(builder.build());
}

folly::SemiFuture<std::vector<fbnl::Route>>
NetlinkProtocolSocket::getIPv4Routes(uint8_t protocolId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default v4 route
  builder.setDestination({folly::IPAddressV4("0.0.0.0"), 0});
  // Set protocol ID
  builder.setProtocolId(protocolId);
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  return getRoutes(builder.build());
}

folly::SemiFuture<std::vector<fbnl::Route>>
NetlinkProtocolSocket::getIPv6Routes(uint8_t protocolId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default v6 route
  builder.setDestination({folly::IPAddressV6("::"), 0});
  // Set protocol ID
  builder.setProtocolId(protocolId);
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  return getRoutes(builder.build());
}

folly::SemiFuture<std::vector<fbnl::Route>>
NetlinkProtocolSocket::getMplsRoutes(uint8_t protocolId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default label
  builder.setMplsLabel(0);
  // Set protocol ID
  builder.setProtocolId(protocolId);
  return getRoutes(builder.build());
}

} // namespace openr::fbnl
