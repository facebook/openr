/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/futures/Future.h>

#include <openr/nl/NetlinkMessage.h>
#include <openr/nl/NetlinkRoute.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr::fbnl {

// Receive socket buffer for netlink socket
constexpr uint32_t kNetlinkSockRecvBuf{1 * 1024 * 1024};

// Maximum number of in-flight messages. `kMinIovMsg` indicates the soft
// requirement for sending bufferred messages.
constexpr size_t kMaxIovMsg{500};
constexpr size_t kMinIovMsg{200};

// Timeout for an ack from kernel for netlink messages we sent. The response for
// big request (e.g. adding 5k routes or getting 10k routes) is sent back in
// multiple parts. If we don't receive any part of below specified timeout, we
// assume kernel is not responsive.
constexpr std::chrono::milliseconds kNlRequestAckTimeout{1000};

/**
 * TODO: Document this class
 *
 * Add/Del APIs returns int value. 0 indicates success and in case of failure,
 * corresponding netlink error code. You can use `nl_geterror(errno)` to get
 * corresponding error string.
 */
class NetlinkProtocolSocket {
 public:
  explicit NetlinkProtocolSocket(fbzmq::ZmqEventLoop* evl);

  // TODO: This should be private and auto initialized (either lazy or static)
  // create socket and add to eventloop
  void init();

  ~NetlinkProtocolSocket();

  // Set netlinkSocket Link event callback
  void setLinkEventCB(std::function<void(fbnl::Link, bool)> linkEventCB);

  // Set netlinkSocket Addr event callback
  void setAddrEventCB(std::function<void(fbnl::IfAddress, bool)> addrEventCB);

  // Set netlinkSocket Addr event callback
  void setNeighborEventCB(
      std::function<void(fbnl::Neighbor, bool)> neighborEventCB);

  /**
   * Add IPv4/IPv6 route. If route entry exists then paths will be appended else
   * new route entry will be created and paths will be added. Returns EXISTS
   * error code if one of the path already exists.
   *
   * @returns 0 on success else appropriate system error code
   */
  folly::SemiFuture<int> addRoute(const openr::fbnl::Route& route);

  /**
   * Delete IPv4/IPv6 route. APIs allows to specifically delete paths of route
   * or whole route itself. If route contains no nexthops, then all the
   * associated paths are removed.
   *
   * @returns 0 on success else appropriate system error code
   */
  folly::SemiFuture<int> deleteRoute(const openr::fbnl::Route& route);

  /**
   * Add MPLS, aka label, route. Nexthops can be PUSH, SWAP, PHP or POP
   *
   * @returns 0 on success else appropriate system error code
   */
  folly::SemiFuture<int> addLabelRoute(const openr::fbnl::Route& route);

  /**
   * Delete MPLS, aka label, route
   *
   * @returns 0 on success else appropriate system error code
   */
  folly::SemiFuture<int> deleteLabelRoute(const openr::fbnl::Route& route);

  /**
   * Add an address to the interface
   *
   * @returns 0 on success else appropriate system error code
   */
  folly::SemiFuture<int> addIfAddress(const openr::fbnl::IfAddress& ifAddr);

  /**
   * Delete an address from the interface
   *
   * @returns 0 on success else appropriate system error code
   */
  folly::SemiFuture<int> deleteIfAddress(const openr::fbnl::IfAddress& ifAddr);

  /**
   * API to get interfaces from kernel
   */
  folly::SemiFuture<std::vector<fbnl::Link>> getAllLinks();

  /**
   * API to get interface addresses from kernel
   */
  folly::SemiFuture<std::vector<fbnl::IfAddress>> getAllIfAddresses();

  /**
   * API to get neighbors from kernel
   */
  folly::SemiFuture<std::vector<fbnl::Neighbor>> getAllNeighbors();

  /**
   * API to retrieve routes from kernel. Attributes specified in filter will be
   * used to selectively retrieve routes.
   */
  folly::SemiFuture<std::vector<fbnl::Route>> getRoutes(
      const fbnl::Route& filter);

  /**
   * APIs to retrieve routes from default routing table.
   * std::vector<fbnl::Route> getAllRoutes();
   */
  folly::SemiFuture<std::vector<fbnl::Route>> getAllRoutes();

  // TODO: Provide thread safe API for interface name <-> index mapping
  // TODO: Provide API to sync route

  /**
   * Utility function to accumulate result of multiple requests into one. The
   * result will be 0 if all the futures are successful else it will contains
   * the first non-zero value (aka error code), in given sequence.
   */
  static folly::SemiFuture<int> collectReturnStatus(
      std::vector<folly::SemiFuture<int>>&& futures,
      std::unordered_set<int> ignoredErrors = {});

 private:
  NetlinkProtocolSocket(NetlinkProtocolSocket const&) = delete;
  NetlinkProtocolSocket& operator=(NetlinkProtocolSocket const&) = delete;

  // Buffer netlink message to the queue_. Invoke sendNetlinkMessage if there
  // are no messages in flight
  void addNetlinkMessage(std::unique_ptr<NetlinkMessage> nlmsg);

  // Send a message batch to netlink socket from queue_
  void sendNetlinkMessage();

  // Receive messages from netlink socket. Invoke `processMessage` for every
  // message received.
  void recvNetlinkMessage();

  // Process received netlink message. Set return values for pending requests
  // or send notifications.
  void processMessage(
      const std::array<char, kMaxNlPayloadSize>& rxMsg, uint32_t bytesRead);

  // Process ack message. Set return status on pending requests in nlSeqNumMap_
  // Resume sending messages from queue_ if any pending
  void processAck(uint32_t ack, int status);

  // Event base for serializing read/write requests to netlink socket. Also
  // ensure thread safety of private member variables.
  fbzmq::ZmqEventLoop* evl_{nullptr};

  // TODO: Avoid callback and use queue for notifications
  // Event callbacks
  std::function<void(fbnl::Link, bool)> linkEventCB_;
  std::function<void(fbnl::IfAddress, bool)> addrEventCB_;
  std::function<void(fbnl::Neighbor, bool)> neighborEventCB_;

  // PID Of the process. Each nl socket is associated with PID and every message
  // sent or received contains this PID. This must be unique for each socket
  // an application create across all the processes on the system.
  uint32_t pid_{UINT_MAX};

  // Netlink socket fd. Created when class is constructed. Re-created on timeout
  // when no response is received for any of our pending requests.
  int nlSock_{-1};

  // Next available sequence number to use. It is possible to wrap this around,
  // and should be fine. We put hard check to avoid conflict between pending
  // seq number with next sequence number.
  // NOTE: We intentionally start from sequence from 1 and not 0. Notification
  // messages from kernel are not associated with any sequence number and they
  // have `nlmsg_seq` set to `0`. There are two message exchanges over nlSock.
  // 1) REQ-REP (for querying data e.g. links/routes from kernel) -- Here we
  //    send request with non-zero sequence number. The messages sent from
  //    kernel in reply will bear the appropriate sequence numbers
  // 2) PUSH (notification message from kernel) -- This notification is from
  //    kernel on any event. There is no sequence number associated with it and
  //    value of nlh->nlmsg_seq will set to 0.
  uint32_t nextNlSeqNum_{1};

  // Netlink message queue. Every add/del/get call for route/addr/neighbor/link
  // translates into one or more NetlinkMessages. These messages are first
  // stored in the queue and sent to kernel in rate limiting fashion. When ack
  // for in-flight messages is received, subsequent messages are sent.
  std::queue<std::unique_ptr<NetlinkMessage>> msgQueue_;

  // Sequence number to NetlinkMesage request mapping. Each in-flight message
  // sent to kernel, is assigned a unique sequence-number and stored in this
  // map. On receipt of ack from kernel (either success or error) we clear the
  // corresponding entry from this map.
  std::unordered_map<uint32_t, std::shared_ptr<NetlinkMessage>> nlSeqNumMap_;

  // Timer to help keep track of timeout of messages sent to kernel. It also
  // ensures the aliveness of the netlink socket-fd. Timer is
  // - Started when a new message is sent
  // - Reset whenever we receive update about one of the pending ack
  // - Cleared when there is no pending ack in nlSeqNumMap_
  // When timer fires, it is an indication that we didn't receive the ack for
  // one of the entry in nlSeqNoMap, for at-least past kNlRequestAckTimeout
  // time. Netlink socket is re-initiaited on timeout for any of our pending
  // message, and `nlSeqNumMap_` is cleared.
  std::unique_ptr<fbzmq::ZmqTimeout> nlMessageTimer_{nullptr};
};

} // namespace openr::fbnl
