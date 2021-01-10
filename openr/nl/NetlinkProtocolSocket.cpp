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

using facebook::fb303::fbData;
namespace fb303 = facebook::fb303;

namespace openr::fbnl {

NetlinkProtocolSocket::NetlinkProtocolSocket(
    folly::EventBase* evb,
    messaging::ReplicateQueue<NetlinkEvent>& netlinkEventsQ,
    bool enableIPv6RouteReplaceSemantics)
    : EventHandler(evb),
      evb_(evb),
      netlinkEventsQueue_(netlinkEventsQ),
      enableIPv6RouteReplaceSemantics_(enableIPv6RouteReplaceSemantics) {
  CHECK_NOTNULL(evb_);

  nlMessageTimer_ = folly::AsyncTimeout::make(*evb_, [this]() noexcept {
    DCHECK(false) << "This shouldn't occur usually. Adding DCHECK to get "
                  << "attention in UTs";

    fbData->addStatValue(
        "netlink.requests.timeout", nlSeqNumMap_.size(), fb303::SUM);

    LOG(ERROR) << "Timed-out receiving ack for " << nlSeqNumMap_.size()
               << " message(s).";
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    for (auto& kv : nlSeqNumMap_) {
      LOG(ERROR) << "  Pending seq=" << kv.first << ", message-type="
                 << static_cast<int>(kv.second->getMessageType())
                 << ", message-size=" << kv.second->getDataLength();
      // Set timeout to pending request
      kv.second->setReturnStatus(-ETIMEDOUT);
    }
    nlSeqNumMap_.clear(); // Clear all timed out requests

    LOG(INFO) << "Closing netlink socket. fd=" << nlSock_
              << ", port=" << portId_;
    unregisterHandler();
    close(nlSock_);
    init();

    // Resume sending netlink messages if any queued
    sendNetlinkMessage();
  });

  // Create consumer for procesing netlink messages to be sent in an event loop
  notifConsumer_ =
      folly::NotificationQueue<std::unique_ptr<NetlinkMessage>>::Consumer::make(
          [this](std::unique_ptr<NetlinkMessage>&& nlmsg) noexcept {
            msgQueue_.push(std::move(nlmsg));
            // Invoke send messages API if socket is initialized and no in
            // flight messages
            if (nlSock_ >= 0 && !nlMessageTimer_->isScheduled()) {
              sendNetlinkMessage();
            }
          });

  // Initialize the socket in an event loop
  nlInitTimer_ = folly::AsyncTimeout::schedule(
      std::chrono::milliseconds(0), *evb_, [this]() noexcept {
        init();
        notifConsumer_->startConsuming(evb_, &notifQueue_);
      });
}

NetlinkProtocolSocket::~NetlinkProtocolSocket() {
  LOG(INFO) << "Shutting down netlink protocol socket";

  // Clear all requests expecting a reply
  for (auto& kv : nlSeqNumMap_) {
    LOG(WARNING) << "Clearing netlink request. seq=" << kv.first
                 << ", message-type=" << kv.second->getMessageType()
                 << ", message-size=" << kv.second->getDataLength();
    // Set timeout to pending request
    kv.second->setReturnStatus(-ESHUTDOWN);
  }
  nlSeqNumMap_.clear(); // Clear all timed out requests

  // Clear all requests that yet needs to be sent
  std::unique_ptr<NetlinkMessage> msg;
  while (notifQueue_.tryConsume(msg)) {
    CHECK_NOTNULL(msg.get());
    LOG(WARNING) << "Clearing netlink message, not yet send";
    msg->setReturnStatus(-ESHUTDOWN);
  }

  LOG(INFO) << "Closing netlink socket. fd=" << nlSock_ << ", port=" << portId_;
  close(nlSock_);
}

void
NetlinkProtocolSocket::init() {
  // Create netlink socket
  nlSock_ = ::socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (nlSock_ < 0) {
    LOG(FATAL) << "Netlink socket create failed.";
  }
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
  LOG(INFO) << "Created netlink socket. fd=" << nlSock_ << ", port=" << portId_;

  // Set fd in event handler and register for polling
  // NOTE: We mask `READ` event with `PERSIST` to make sure the handler remains
  // registered after the read event
  LOG(INFO) << "Registering netlink socket fd " << nlSock_
            << " with EventBase for read events";
  changeHandlerFD(folly::NetworkSocket{nlSock_});
  registerHandler(folly::EventHandler::READ | folly::EventHandler::PERSIST);

  // Resume sending netlink messages if any queued
  sendNetlinkMessage();
}

void
NetlinkProtocolSocket::handlerReady(uint16_t events) noexcept {
  CHECK_EQ(events, folly::EventHandler::READ);
  try {
    recvNetlinkMessage();
  } catch (std::exception const& e) {
    LOG(ERROR) << "Error processing netlink message" << folly::exceptionStr(e);
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
  }
}

void
NetlinkProtocolSocket::setLinkEventCB(
    std::function<void(fbnl::Link, bool)> linkEventCB) {
  CHECK(!linkEventCB_) << "Callback can be registered only once";
  linkEventCB_ = linkEventCB;
}

void
NetlinkProtocolSocket::setAddrEventCB(
    std::function<void(fbnl::IfAddress, bool)> addrEventCB) {
  CHECK(!addrEventCB_) << "Callback can be registered only once";
  addrEventCB_ = addrEventCB;
}

void
NetlinkProtocolSocket::setNeighborEventCB(
    std::function<void(fbnl::Neighbor, bool)> neighborEventCB) {
  CHECK(!neighborEventCB_) << "Callback can be registered only once";
  neighborEventCB_ = neighborEventCB;
}

void
NetlinkProtocolSocket::processAck(uint32_t ack, int status) {
  VLOG(2) << "Completed netlink request. seq=" << ack << ", retval=" << status;
  if (std::abs(status) != EEXIST && std::abs(status) != ESRCH && status != 0) {
    fbData->addStatValue("netlink.requests.error", 1, fb303::SUM);
  } else {
    fbData->addStatValue("netlink.requests.success", 1, fb303::SUM);
  }

  auto it = nlSeqNumMap_.find(ack);
  if (it != nlSeqNumMap_.end()) {
    // Calculate and add the latency of the request in fb303
    auto requestLatency = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - it->second->getCreateTs());
    fbData->addStatValue(
        "netlink.requests.latency_ms", requestLatency.count(), fb303::AVG);

    // Set return status on promise
    it->second->setReturnStatus(status);
    nlSeqNumMap_.erase(it);
  } else {
    LOG(ERROR) << "Broken promise for netlink request. seq=" << ack;
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
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
  CHECK(evb_->isInEventBaseThread());
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
      fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    }

    // Add seq number -> netlink request mapping
    auto res = nlSeqNumMap_.insert({nlmsg_hdr->nlmsg_seq, std::move(m)});
    CHECK(res.second) << "Entry exists for " << nlmsg_hdr->nlmsg_seq;
    count++;
    VLOG(2) << "Sending netlink request."
            << " seq=" << nlmsg_hdr->nlmsg_seq
            << ", type=" << nlmsg_hdr->nlmsg_type
            << ", len=" << nlmsg_hdr->nlmsg_len
            << ", flags=" << nlmsg_hdr->nlmsg_flags;
  }

  auto outMsg = std::make_unique<struct msghdr>();
  outMsg->msg_name = &nladdr;
  outMsg->msg_namelen = sizeof(nladdr);
  outMsg->msg_iov = &iov[0];
  outMsg->msg_iovlen = count;

  // `sendmsg` return -1 in case of error else number of bytes sent. `errno`
  // will be set to an appropriate code in case of error.
  int bytesSent = sendmsg(nlSock_, outMsg.get(), 0);
  if (bytesSent < 0) {
    LOG(ERROR) << "Error sending on netlink socket. Error: "
               << folly::errnoStr(std::abs(errno)) << ", errno=" << errno
               << ", fd=" << nlSock_ << ", num-messages=" << outMsg->msg_iovlen;
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
  } else {
    fbData->addStatValue("netlink.bytes.tx", bytesSent, fb303::SUM);
  }
  fbData->addStatValue("netlink.requests", outMsg->msg_iovlen, fb303::SUM);
  VLOG(2) << "Sent " << outMsg->msg_iovlen << " netlink requests on fd "
          << nlSock_;

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

    VLOG(2) << "Received reply for netlink request."
            << " seq=" << nlh->nlmsg_seq << ", type=" << nlh->nlmsg_type
            << ", len=" << nlh->nlmsg_len << ", flags=" << nlh->nlmsg_flags;
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
        fbData->addStatValue("netlink.notifications.route", 1, fb303::SUM);
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
        VLOG(1) << "Link event. " << link.str();
        fbData->addStatValue("netlink.notifications.link", 1, fb303::SUM);
        if (linkEventCB_) {
          linkEventCB_(link, true);
        }

        // notification via replicateQueue
        netlinkEventsQueue_.push(link);
      }
    } break;

    case RTM_DELADDR:
    case RTM_NEWADDR: {
      // process interface address information received from netlink
      auto addr = NetlinkAddrMessage::parseMessage(nlh);
      if (not addr.getPrefix().has_value()) {
        LOG(WARNING) << "Address event with empty address: " << addr.str();
        break;
      }

      bool isNotification = nlSeqIt == nlSeqNumMap_.end();
      if (nlSeqIt != nlSeqNumMap_.end()) {
        // Extend message timer as we received a valid ack
        nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
        // Response to a corresponding request
        auto& request = nlSeqIt->second;
        if (request->getMessageType() == RTM_GETADDR) {
          // Received link in response to request
          request->rcvdIfAddress(std::move(addr));
        } else {
          // Response to a add/del request - generate addr event for handler.
          // This occurs when we add/del IPv4 addresses generates address event
          // with the same sequence as the original request.
          isNotification = true;
        }
      }

      if (isNotification) {
        // IfAddress notification
        VLOG(1) << "Address event. " << addr.str();
        fbData->addStatValue("netlink.notifications.addr", 1, fb303::SUM);
        if (addrEventCB_) {
          addrEventCB_(addr, true);
        }

        // notification via replicateQueue
        netlinkEventsQueue_.push(addr);
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
        VLOG(2) << "Netlink neighbor event. " << neighbor.str();
        fbData->addStatValue("netlink.notifications.neighbor", 1, fb303::SUM);
        if (neighborEventCB_) {
          neighborEventCB_(neighbor, true);
        }

        // notification via replicateQueue
        netlinkEventsQueue_.push(neighbor);
      }
    } break;

    case NLMSG_ERROR: {
      const struct nlmsgerr* const ack =
          reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(nlh));
      if (ack->msg.nlmsg_pid != portId_) {
        LOG(ERROR) << "received netlink message with wrong PID, received: "
                   << ack->msg.nlmsg_pid << " expected: " << portId_;
        fbData->addStatValue("netlink.errors", 1, fb303::SUM);
        break;
      }
      processAck(ack->msg.nlmsg_seq, ack->error);
    } break;

    case NLMSG_NOOP:
      // Message is to be ignored as per netlink spec
      break;

    case NLMSG_DONE: {
      // End of multipart message
      processAck(nlh->nlmsg_seq, 0);
    } break;

    default:
      LOG(ERROR) << "Unknown message type: " << nlh->nlmsg_type;
      fbData->addStatValue("netlink.errors", 1, fb303::SUM);
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
    LOG(ERROR) << "Error in netlink socket receive: " << bytesRead
               << " err: " << folly::errnoStr(std::abs(errno));
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    return;
  } else {
    fbData->addStatValue("netlink.bytes.rx", bytesRead, fb303::SUM);
  }
  processMessage(recvMsg, static_cast<uint32_t>(bytesRead));
}

folly::SemiFuture<int>
NetlinkProtocolSocket::collectReturnStatus(
    std::vector<folly::SemiFuture<int>>&& futures,
    std::unordered_set<int> ignoredErrors) {
  return folly::collectAll(std::move(futures))
      .defer(
          [ignoredErrors](folly::Try<std::vector<folly::Try<int>>>&& results) {
            for (auto& result : results.value()) {
              auto retval = std::abs(result.value()); // Throws exeption if any
              if (retval == 0 or ignoredErrors.count(retval)) {
                continue;
              }

              // We encountered first non zero value. Report error and return
              LOG(ERROR) << "One or more Netlink requests failed with error: "
                         << retval << " -- " << folly::errnoStr(retval);
              return retval;
            }
            return 0;
          });
}

folly::SemiFuture<int>
NetlinkProtocolSocket::addRoute(const openr::fbnl::Route& route) {
  VLOG(1) << "Netlink add route. " << route.str();
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
    notifQueue_.putMessage(std::move(rtmMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::deleteRoute(const openr::fbnl::Route& route) {
  VLOG(1) << "Netlink delete route. " << route.str();
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
    notifQueue_.putMessage(std::move(rtmMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::addIfAddress(const openr::fbnl::IfAddress& ifAddr) {
  VLOG(1) << "Netlink add interface address. " << ifAddr.str();
  auto addrMsg = std::make_unique<openr::fbnl::NetlinkAddrMessage>();
  auto future = addrMsg->getSemiFuture();

  // Initialize Netlink message fields to add interface address
  int status = addrMsg->addOrDeleteIfAddress(ifAddr, RTM_NEWADDR);
  if (status != 0) {
    addrMsg->setReturnStatus(status);
  } else {
    notifQueue_.putMessage(std::move(addrMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::deleteIfAddress(const openr::fbnl::IfAddress& ifAddr) {
  VLOG(1) << "Netlink delete interface address. " << ifAddr.str();
  auto addrMsg = std::make_unique<openr::fbnl::NetlinkAddrMessage>();
  auto future = addrMsg->getSemiFuture();

  // Initialize Netlink message fields to delete interface address
  int status = addrMsg->addOrDeleteIfAddress(ifAddr, RTM_DELADDR);
  if (status != 0) {
    addrMsg->setReturnStatus(status);
  } else {
    notifQueue_.putMessage(std::move(addrMsg));
  }

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Link>, int>>
NetlinkProtocolSocket::getAllLinks() {
  VLOG(1) << "Netlink get links";
  auto linkMsg = std::make_unique<openr::fbnl::NetlinkLinkMessage>();
  auto future = linkMsg->getLinksSemiFuture();

  // Initialize message fields to get all links
  linkMsg->init(RTM_GETLINK, 0);
  notifQueue_.putMessage(std::move(linkMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::IfAddress>, int>>
NetlinkProtocolSocket::getAllIfAddresses() {
  VLOG(1) << "Netlink get interface addresses";
  auto addrMsg = std::make_unique<openr::fbnl::NetlinkAddrMessage>();
  auto future = addrMsg->getAddrsSemiFuture();

  // Initialize message fields to get all addresses
  addrMsg->init(RTM_GETADDR);
  notifQueue_.putMessage(std::move(addrMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Neighbor>, int>>
NetlinkProtocolSocket::getAllNeighbors() {
  VLOG(1) << "Netlink get neighbors";
  auto neighMsg = std::make_unique<openr::fbnl::NetlinkNeighborMessage>();
  auto future = neighMsg->getNeighborsSemiFuture();

  // Initialize message fields to get all neighbors
  neighMsg->init(RTM_GETNEIGH, 0);
  notifQueue_.putMessage(std::move(neighMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getRoutes(const fbnl::Route& filter) {
  VLOG(1) << "Netlink get routes with filter. " << filter.str();
  auto routeMsg = std::make_unique<openr::fbnl::NetlinkRouteMessage>();
  auto future = routeMsg->getRoutesSemiFuture();

  // Initialize message fields to get all addresses
  routeMsg->init(RTM_GETROUTE, 0, filter);
  notifQueue_.putMessage(std::move(routeMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getAllRoutes() {
  fbnl::RouteBuilder builder;
  builder.setProtocolId(RTPROT_UNSPEC); // Explicitly set protocol to 0
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  return getRoutes(builder.build());
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getIPv4Routes(uint8_t protocolId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default v4 route
  builder.setDestination({folly::IPAddressV4("0.0.0.0"), 0});
  // Set protocol ID
  builder.setProtocolId(protocolId);
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  return getRoutes(builder.build());
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getIPv6Routes(uint8_t protocolId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default v6 route
  builder.setDestination({folly::IPAddressV6("::"), 0});
  // Set protocol ID
  builder.setProtocolId(protocolId);
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  return getRoutes(builder.build());
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getMplsRoutes(uint8_t protocolId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default label
  builder.setMplsLabel(0);
  // Set protocol ID
  builder.setProtocolId(protocolId);
  return getRoutes(builder.build());
}

} // namespace openr::fbnl
