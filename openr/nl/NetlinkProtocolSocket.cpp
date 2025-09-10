/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>

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
  // We expect ctrl-evb not be running. Attaching and scheduling
  // of timers is not thread safe.
  CHECK_NOTNULL(evb_);
  CHECK(!evb_->isRunning());

  nlMessageTimer_ = folly::AsyncTimeout::make(*evb_, [this]() noexcept {
    DCHECK(false) << "This shouldn't occur usually. Adding DCHECK to get "
                  << "attention in UTs";

    fbData->addStatValue(
        "netlink.requests.timeout", nlSeqNumMap_.size(), fb303::SUM);

    XLOG(ERR) << "Timed-out receiving ack for " << nlSeqNumMap_.size()
              << " message(s).";
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    for (auto& kv : nlSeqNumMap_) {
      XLOG(ERR) << "  Pending seq=" << kv.first << ", message-type="
                << static_cast<int>(kv.second->getMessageType())
                << ", message-size=" << kv.second->getDataLength();
      // Set timeout to pending request
      kv.second->setReturnStatus(-ETIMEDOUT);
    }
    nlSeqNumMap_.clear(); // Clear all timed out requests

    XLOG(INFO) << "Closing netlink socket. fd=" << nlSock_
               << ", port=" << portId_;
    unregisterHandler();
    close(nlSock_);
    init();

    // Resume sending netlink messages if any queued
    sendNetlinkMessage();
  });

  // Create consumer for procesing netlink messages to be sent in an event loop
  notifConsumer_ =
      folly::NotificationQueue<std::unique_ptr<NetlinkMessageBase>>::Consumer::
          make([this](std::unique_ptr<NetlinkMessageBase>&& nlmsg) noexcept {
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
  XLOG(INFO) << "Shutting down netlink protocol socket";

  // Clear all requests expecting a reply
  for (auto& kv : nlSeqNumMap_) {
    XLOG(WARNING) << "Clearing netlink request. seq=" << kv.first
                  << ", message-type=" << kv.second->getMessageType()
                  << ", message-size=" << kv.second->getDataLength();
    // Set timeout to pending request
    kv.second->setReturnStatus(-ESHUTDOWN);
  }
  nlSeqNumMap_.clear(); // Clear all timed out requests

  // Clear all requests that yet needs to be sent
  std::unique_ptr<NetlinkMessageBase> msg;
  while (notifQueue_.tryConsume(msg)) {
    CHECK_NOTNULL(msg.get());
    XLOG(WARNING) << "Clearing netlink message, not yet send";
    msg->setReturnStatus(-ESHUTDOWN);
  }

  if (nlSock_ > 0) {
    XLOG(INFO) << "Closing netlink socket. fd=" << nlSock_
               << ", port=" << portId_;
    close(nlSock_);
  } else {
    XLOG(INFO) << "Netlink socket was never initialized";
  }
}

void
NetlinkProtocolSocket::init() {
  // Create netlink socket
  nlSock_ = ::socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (nlSock_ < 0) {
    XLOG(FATAL) << "Netlink socket create failed.";
  }
  int size = kNetlinkSockRecvBuf;
  // increase socket recv buffer size
  if (setsockopt(nlSock_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
    XLOG(FATAL) << "Netlink socket set recv buffer failed.";
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
    XLOG(FATAL) << "Failed to bind netlink socket: " << folly::errnoStr(errno);
  }

  // Retrieve and set pid that we will use for all subsequent messages
  portId_ = saddr.nl_pid;
  XLOG(INFO) << "Created netlink socket. fd=" << nlSock_
             << ", port=" << portId_;

  // Set fd in event handler and register for polling
  // NOTE: We mask `READ` event with `PERSIST` to make sure the handler remains
  // registered after the read event
  XLOG(INFO) << "Registering netlink socket fd " << nlSock_
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
    XLOG(ERR) << "Error processing netlink message" << folly::exceptionStr(e);
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
  }
}

void
NetlinkProtocolSocket::processAck(uint32_t ack, int status) {
  XLOG(DBG2) << "Completed netlink request. seq=" << ack
             << ", retval=" << status;
  if (std::abs(status) != EEXIST && std::abs(status) != ESRCH && status != 0) {
    XLOG(ERR) << "Netlink request error for seq=" << ack
              << ", retval=" << status;
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
    XLOG(ERR) << "Broken promise for netlink request. seq=" << ack;
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
  if (nlSeqNumMap_.empty() || (kMaxIovMsg - nlSeqNumMap_.size() > kMinIovMsg)) {
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
      XLOG(ERR) << "Error: multipart netlink message not supported";
      fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    }

    // Add seq number -> netlink request mapping
    auto res = nlSeqNumMap_.insert({nlmsg_hdr->nlmsg_seq, std::move(m)});
    CHECK(res.second) << "Entry exists for " << nlmsg_hdr->nlmsg_seq;
    count++;
    XLOG(DBG2) << "Sending netlink request." << " seq=" << nlmsg_hdr->nlmsg_seq
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
    XLOG(ERR) << "Error sending on netlink socket. Error: "
              << folly::errnoStr(std::abs(errno)) << ", errno=" << errno
              << ", fd=" << nlSock_ << ", num-messages=" << outMsg->msg_iovlen;
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
  } else {
    fbData->addStatValue("netlink.bytes.tx", bytesSent, fb303::SUM);
  }
  fbData->addStatValue("netlink.requests", outMsg->msg_iovlen, fb303::SUM);
  XLOG(DBG2) << "Sent " << outMsg->msg_iovlen << " netlink requests on fd "
             << nlSock_;

  // Schedule timer to wait for acks and send next set of messages
  nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
}

void
NetlinkProtocolSocket::processMessage(
    const std::array<char, kNetlinkSockRecvBuf>& rxMsg, uint32_t bytesRead) {
  // first netlink message header
  struct nlmsghdr* nlh = (struct nlmsghdr*)rxMsg.data();
  do {
    if (!NLMSG_OK(nlh, bytesRead)) {
      break;
    }

    XLOG(DBG2) << "Received reply for netlink request."
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
        XLOG(DBG1) << "Link event. " << link.str();
        fbData->addStatValue("netlink.notifications.link", 1, fb303::SUM);
        netlinkEventsQueue_.push(link);
      }
    } break;

    case RTM_DELADDR:
    case RTM_NEWADDR: {
      // process interface address information received from netlink
      auto addr = NetlinkAddrMessage::parseMessage(nlh);
      if (!addr.getPrefix().has_value()) {
        XLOG(WARNING) << "Address event with empty address: " << addr.str();
        break;
      }

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

          // IfAddress notification
          XLOG(DBG1) << "Address event. " << addr.str();
          fbData->addStatValue("netlink.notifications.addr", 1, fb303::SUM);
          netlinkEventsQueue_.push(addr);
        }
      } else {
        // IfAddress notification
        XLOG(DBG1) << "Address event. " << addr.str();
        fbData->addStatValue("netlink.notifications.addr", 1, fb303::SUM);
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
        XLOG(DBG2) << "Neighbor event. " << neighbor.str();
        fbData->addStatValue("netlink.notifications.neighbor", 1, fb303::SUM);
        netlinkEventsQueue_.push(neighbor);
      }
    } break;

    case RTM_DELRULE:
    case RTM_NEWRULE: {
      // process rule information received from netlink
      auto rule = NetlinkRuleMessage::parseMessage(nlh);

      if (nlSeqIt != nlSeqNumMap_.end()) {
        // Extend message timer as we received a valid ack
        nlMessageTimer_->scheduleTimeout(kNlRequestAckTimeout);
        // Received rule in response to request
        nlSeqIt->second->rcvdRule(std::move(rule));
      } else {
        // Rule notification
        XLOG(DBG2) << "Rule event. " << rule.str();
        fbData->addStatValue("netlink.notifications.rule", 1, fb303::SUM);
        netlinkEventsQueue_.push(rule);
      }
    } break;

    case NLMSG_ERROR: {
      const struct nlmsgerr* const ack =
          reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(nlh));
      if (ack->msg.nlmsg_pid != portId_) {
        XLOG(ERR) << "received netlink message with wrong PID, received: "
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
      XLOG(ERR) << "Unknown message type: " << nlh->nlmsg_type;
      fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    }
  } while ((nlh = NLMSG_NEXT(nlh, bytesRead)));
}

void
NetlinkProtocolSocket::recvNetlinkMessage() {
  // messages buffer, set the size same as what is set as receive
  // buffer size for nlSock_
  std::array<char, kNetlinkSockRecvBuf> recvMsg = {};

  int32_t bytesRead = ::recv(nlSock_, recvMsg.data(), kNetlinkSockRecvBuf, 0);
  XLOG(DBG4) << "Message received with size: " << bytesRead;

  if (bytesRead < 0) {
    if (errno == EINTR || errno == EAGAIN) {
      return;
    }
    XLOG(ERR) << "Error in netlink socket receive: " << bytesRead
              << " err: " << folly::errnoStr(std::abs(errno));
    fbData->addStatValue("netlink.errors", 1, fb303::SUM);
    return;
  } else {
    fbData->addStatValue("netlink.bytes.rx", bytesRead, fb303::SUM);
  }
  processMessage(recvMsg, static_cast<uint32_t>(bytesRead));
}

folly::SemiFuture<folly::Unit>
NetlinkProtocolSocket::collectReturnStatus(
    std::vector<folly::SemiFuture<int>>&& futures,
    std::unordered_set<int> ignoredErrors) {
  return folly::collectAll(std::move(futures))
      .defer([ignoredErrors](
                 folly::Try<std::vector<folly::Try<int>>>&& results) {
        for (auto& result : results.value()) {
          auto retval = std::abs(result.value()); // Throws exeption if any
          if (retval == 0 || ignoredErrors.count(retval)) {
            continue;
          }
          throw fbnl::NlException("One or more netlink request failed", retval);
        }
        return folly::Unit();
      });
}

folly::SemiFuture<int>
NetlinkProtocolSocket::addRoute(const openr::fbnl::Route& route) {
  XLOG(DBG1) << "Netlink add route. " << route.str();
  auto rtmMsg = std::make_unique<NetlinkRouteMessage>();
  auto future = rtmMsg->getSemiFuture();

  int status{0};
  switch (route.getFamily()) {
  case AF_INET6:
    if (!enableIPv6RouteReplaceSemantics_) {
      // Special case for IPv6 route add. We first delete the route and then
      // add it.
      // NOTE: We ignore the error for the deleteRoute
      deleteRoute(route);
    }
    [[fallthrough]];
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
  XLOG(DBG1) << "Netlink delete route. " << route.str();
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
  XLOG(DBG1) << "Netlink add interface address. " << ifAddr.str();
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
  XLOG(DBG1) << "Netlink delete interface address. " << ifAddr.str();
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

folly::SemiFuture<int>
NetlinkProtocolSocket::addLink(const openr::fbnl::Link& link) {
  XLOG(DBG1) << "Netlink add link. " << link.str();
  auto linkMsg = std::make_unique<openr::fbnl::NetlinkLinkMessage>();
  auto future = linkMsg->getSemiFuture();

  int status = linkMsg->addLink(link);
  if (status != 0) {
    linkMsg->setReturnStatus(status);
  } else {
    notifQueue_.putMessage(std::move(linkMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::deleteLink(const openr::fbnl::Link& link) {
  XLOG(DBG1) << "Netlink delete link. " << link.str();
  auto linkMsg = std::make_unique<openr::fbnl::NetlinkLinkMessage>();
  auto future = linkMsg->getSemiFuture();

  int status = linkMsg->deleteLink(link);
  if (status != 0) {
    linkMsg->setReturnStatus(status);
  } else {
    notifQueue_.putMessage(std::move(linkMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::addRule(const openr::fbnl::Rule& rule) {
  XLOG(DBG1) << "Netlink add rule. " << rule.str();
  auto ruleMsg = std::make_unique<openr::fbnl::NetlinkRuleMessage>();
  auto future = ruleMsg->getSemiFuture();

  int status = ruleMsg->addRule(rule);
  if (status != 0) {
    ruleMsg->setReturnStatus(status);
  } else {
    notifQueue_.putMessage(std::move(ruleMsg));
  }

  return future;
}

folly::SemiFuture<int>
NetlinkProtocolSocket::deleteRule(const openr::fbnl::Rule& rule) {
  XLOG(DBG1) << "Netlink delete rule. " << rule.str();
  auto ruleMsg = std::make_unique<openr::fbnl::NetlinkRuleMessage>();
  auto future = ruleMsg->getSemiFuture();

  int status = ruleMsg->deleteRule(rule);
  if (status != 0) {
    ruleMsg->setReturnStatus(status);
  } else {
    notifQueue_.putMessage(std::move(ruleMsg));
  }

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Link>, int>>
NetlinkProtocolSocket::getAllLinks() {
  XLOG(DBG3) << "Netlink get links";
  auto linkMsg = std::make_unique<openr::fbnl::NetlinkLinkMessage>();
  auto future = linkMsg->getLinksSemiFuture();

  // Initialize message fields to get all links
  linkMsg->init(RTM_GETLINK, 0);
  notifQueue_.putMessage(std::move(linkMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::IfAddress>, int>>
NetlinkProtocolSocket::getAllIfAddresses() {
  XLOG(DBG3) << "Netlink get interface addresses";
  auto addrMsg = std::make_unique<openr::fbnl::NetlinkAddrMessage>();
  auto future = addrMsg->getAddrsSemiFuture();

  // Initialize message fields to get all addresses
  addrMsg->init(RTM_GETADDR);
  notifQueue_.putMessage(std::move(addrMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Neighbor>, int>>
NetlinkProtocolSocket::getAllNeighbors() {
  XLOG(DBG1) << "Netlink get neighbors";
  auto neighMsg = std::make_unique<openr::fbnl::NetlinkNeighborMessage>();
  auto future = neighMsg->getNeighborsSemiFuture();

  // Initialize message fields to get all neighbors
  neighMsg->init(RTM_GETNEIGH, 0);
  notifQueue_.putMessage(std::move(neighMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Rule>, int>>
NetlinkProtocolSocket::getAllRules() {
  XLOG(DBG1) << "Netlink get rules";
  auto ruleMsg = std::make_unique<openr::fbnl::NetlinkRuleMessage>();
  auto future = ruleMsg->getRulesSemiFuture();

  // Initialize message fields to get all rules
  ruleMsg->init(RTM_GETRULE);
  notifQueue_.putMessage(std::move(ruleMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getRoutes(const fbnl::Route& filter) {
  XLOG(DBG1) << "Netlink get routes with filter. " << filter.str();
  auto routeMsg = std::make_unique<openr::fbnl::NetlinkRouteMessage>();
  auto future = routeMsg->getRoutesSemiFuture();

  // Initialize message fields to get all addresses
  routeMsg->initGet(0, filter);
  notifQueue_.putMessage(std::move(routeMsg));

  return future;
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getAllRoutes(std::optional<uint8_t> routeTableId) {
  fbnl::RouteBuilder builder;
  builder.setProtocolId(RTPROT_UNSPEC); // Explicitly set protocol to 0
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  // Set route table ID if given
  if (routeTableId.has_value()) {
    builder.setRouteTable(routeTableId.value());
  }
  return getRoutes(builder.build());
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getIPv4Routes(
    uint8_t protocolId, std::optional<uint8_t> routeTableId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default v4 route
  builder.setDestination({folly::IPAddressV4("0.0.0.0"), 0});
  // Set protocol ID
  builder.setProtocolId(protocolId);
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  // Set route table ID if given
  if (routeTableId.has_value()) {
    builder.setRouteTable(routeTableId.value());
  }
  return getRoutes(builder.build());
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getIPv6Routes(
    uint8_t protocolId, std::optional<uint8_t> routeTableId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default v6 route
  builder.setDestination({folly::IPAddressV6("::"), 0});
  // Set protocol ID
  builder.setProtocolId(protocolId);
  builder.setType(RTN_UNSPEC); // Explicitly set type to 0
  // Set route table ID if given
  if (routeTableId.has_value()) {
    builder.setRouteTable(routeTableId.value());
  }
  return getRoutes(builder.build());
}

folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
NetlinkProtocolSocket::getMplsRoutes(
    uint8_t protocolId, std::optional<uint8_t> routeTableId) {
  fbnl::RouteBuilder builder;
  // Set address family to MPLS with default label
  builder.setMplsLabel(0);
  // Set protocol ID
  builder.setProtocolId(protocolId);
  // Set route table ID if given
  if (routeTableId.has_value()) {
    builder.setRouteTable(routeTableId.value());
  }
  return getRoutes(builder.build());
}

} // namespace openr::fbnl
