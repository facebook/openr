/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <folly/IPAddress.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventHandler.h>
#include <folly/io/async/NotificationQueue.h>

#include <openr/messaging/ReplicateQueue.h>
#include <openr/nl/NetlinkMessage.h>
#include <openr/nl/NetlinkRoute.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr::fbnl {

// Netlink event as union of LINK/ADDR/NEIGH event
using NetlinkEvent = std::variant<fbnl::Link, fbnl::IfAddress, fbnl::Neighbor>;

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
 * C++ async interface for netlink APIs. It supports minimal functionality that
 * Open/R needs but can be easily extended to support any netlink message
 * exchange.
 *
 * NOTE APIs
 * All public APIs are asynchronous. An API call, transaltes to exactly one
 * message that needs to be sent to kernel. It is enqueued and future associated
 * with request is returned. In response to API call, kernel may send one or
 * more messages followed by an ack. The future is fulfilled when ack is
 * received. Appropriate error code or return value is set. In case of failure
 * the request may timeout and ETIMEOUT will be set.
 *
 * NOTE Threading:
 * EventBase used to initiate this class is used in serializing the messages
 * that needs to be sent to kernel. Further it also polls netlink socket fd
 * for any messages that needs to be read. Received messages may fulfil the
 * future of outstanding requests.
 *
 * NOTE Performance:
 * Above threading model allows multiple requests to be sent in parallel and
 * process their response asynchronously. Outstanding requests to kernel is
 * rate-limited to not overwhelm the socket buffers. Rate-limiting of requests
 * is governed by params kMaxIovMsg and kMinIovMsg. This allows adding 100k
 * routes in under 2 seconds. These performance benchmarks can be observed
 * by running associated UTs and it might vary on different systems.
 *
 * NOTE Logging:
 * Netlink protocol is tricky when it comes to debugging. To faciliate debugging
 * the library supports hierarchical level of logging. All unexpected errors
 * are reported with seviority `ERROR`.
 * INFO - Socket create, close and register events
 * VERBOSE 1 - Logs request objects & their status
 * VERBOSE 2 - Details of netlink request, responses and events. (ack, type,
 *             len, flags etc)
 * VERBOSE 3 - Details of netlink message parsing
 *
 * NOTE Monitoring:
 * This module exposes fb303 counters that can be leveraged for monitoring
 * application's correctness and performance behavior in production
 *   netlink.errors : any LOG(ERROR) will bump this counter
 *   netlink.requests : Sent requests
 *   netlink.requests.timeouts : Timed out requests
 *   netlink.requests.success : Request that completed successfully
 *   netlink.requests.error : Request with non zero return code
 *   netlink.requests.latency_ms : Average latency of netlink request
 *   netlink.bytes.rx : Bytes received over netlink socket
 *   netlink.bytes.tx : Bytes sent over netlink socket
 *   netlink.notifications.link : Received link notifications
 *   netlink.notifications.addr : Received address notifications
 *   netlink.notifications.neighbors : Received neighbor notifications
 *   netlink.notifications.route : Received route notifications
 */
class NetlinkProtocolSocket : public folly::EventHandler {
 public:
  explicit NetlinkProtocolSocket(
      folly::EventBase* evb,
      messaging::ReplicateQueue<NetlinkEvent>& netlinkEventsQ,
      bool enableIPv6RouteReplaceSemantics = false);

  virtual ~NetlinkProtocolSocket();

  // Set netlinkSocket Link event callback
  void setLinkEventCB(std::function<void(fbnl::Link, bool)> linkEventCB);

  // Set netlinkSocket Addr event callback
  void setAddrEventCB(std::function<void(fbnl::IfAddress, bool)> addrEventCB);

  // Set netlinkSocket Addr event callback
  void setNeighborEventCB(
      std::function<void(fbnl::Neighbor, bool)> neighborEventCB);

  /**
   * Add or replace route. An existing paths of route will be replaced with
   * new paths. Supports AF_INET, AF_INET6 and AF_MPLS address families.
   *
   * Either route.getDestination() or route.getMplsLabel() must be set when
   * route is built.
   *
   * NOTE: For AF_INET6 kernel has different behavior for NLM_F_REPLACE. Hence,
   * to provide consistent API interface across all address family, this API
   * first removes the route if destination is IPv6 and add all new paths. There
   * can be a breif period of packet drops when route is deleted and added
   * again. On kernel 4.18+ new IPv6 route replace semantics allows seamless
   * route replace for IPv6. It can be enabeld by constructor parameter.
   *
   * @returns 0 on success else appropriate system error code
   */
  virtual folly::SemiFuture<int> addRoute(const openr::fbnl::Route& route);

  /**
   * Delete route. This API deletes all the paths associated with the route
   * based on key (destination-address or mpls top-label). Supports AF_INET,
   * AF_INET6 and AF_MPLS address families
   *
   * Either route.getDestination() or route.getMplsLabel() must be set when
   * route is built. `nexthops()` attributes are completely ignored.
   *
   * @returns 0 on success else appropriate system error code
   */
  virtual folly::SemiFuture<int> deleteRoute(const openr::fbnl::Route& route);

  /**
   * Add an address to the interface
   *
   * @returns 0 on success else appropriate system error code
   */
  virtual folly::SemiFuture<int> addIfAddress(
      const openr::fbnl::IfAddress& ifAddr);

  /**
   * Delete an address from the interface
   *
   * @returns 0 on success else appropriate system error code
   */
  virtual folly::SemiFuture<int> deleteIfAddress(
      const openr::fbnl::IfAddress& ifAddr);

  /**
   * API to get interfaces from kernel
   */
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::Link>, int>>
  getAllLinks();

  /**
   * API to get interface addresses from kernel.
   */
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::IfAddress>, int>>
  getAllIfAddresses();

  /**
   * API to get neighbors from kernel
   */
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::Neighbor>, int>>
  getAllNeighbors();

  /**
   * API to retrieve routes from kernel. Attributes specified in filter will be
   * used to selectively retrieve routes. Filter is supported on following
   * attributes. 0 will act as wildcard for querying routes.
   * - table
   * - protocol
   * - address family
   * - type
   */
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
  getRoutes(const fbnl::Route& filter);

  /**
   * APIs to retrieve routes from default routing table.
   * std::vector<fbnl::Route> getAllRoutes();
   */
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
  getAllRoutes();
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
  getIPv4Routes(uint8_t protocolId);
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
  getIPv6Routes(uint8_t protocolId);
  virtual folly::SemiFuture<folly::Expected<std::vector<fbnl::Route>, int>>
  getMplsRoutes(uint8_t protocolId);

  /**
   * Utility function to accumulate result of multiple requests into one. The
   * result will be 0 if all the futures are successful else it will contains
   * the first non-zero value (aka error code), in given sequence.
   */
  static folly::SemiFuture<int> collectReturnStatus(
      std::vector<folly::SemiFuture<int>>&& futures,
      std::unordered_set<int> ignoredErrors = {});

 protected:
  // Initialize netlink socket and add to eventloop for polling
  virtual void init();

  // TODO: Avoid callback and use queue for notifications
  // Event callbacks
  std::function<void(fbnl::Link, bool)> linkEventCB_;
  std::function<void(fbnl::IfAddress, bool)> addrEventCB_;
  std::function<void(fbnl::Neighbor, bool)> neighborEventCB_;

 private:
  NetlinkProtocolSocket(NetlinkProtocolSocket const&) = delete;
  NetlinkProtocolSocket& operator=(NetlinkProtocolSocket const&) = delete;

  // Implement EventHandler callback for reading netlink messages
  void handlerReady(uint16_t events) noexcept override;

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
  folly::EventBase* evb_{nullptr};

  // Queue to publish LINK/ADDR/NEIGHBOR update received from kernel
  messaging::ReplicateQueue<NetlinkEvent>& netlinkEventsQueue_;

  // Notification queue for thread safe enqueuing of messages from external
  // threads. All the messages enqueued are processed by the event thread.
  folly::NotificationQueue<std::unique_ptr<NetlinkMessage>> notifQueue_;
  std::unique_ptr<
      folly::NotificationQueue<std::unique_ptr<NetlinkMessage>>::Consumer,
      folly::DelayedDestruction::Destructor>
      notifConsumer_;

  // Use new IPv6 route replace semantics. See documentation for addRoute(...)
  const bool enableIPv6RouteReplaceSemantics_{false};

  // Netlink socket fd. Created when class is constructed. Re-created on timeout
  // when no response is received for any of our pending requests.
  int nlSock_{-1};

  // nl_pid stands for port-ID and not process-ID. Netlink sockets are bound on
  // this specified port. This must be unique for every netlink socket that
  // is created on the system. Ironically kernel assigns the process-ID as the
  // port-ID for the first socket that is created by process. All subsequent
  // netlink sockets created by process gets assigned some unique-ID.
  uint32_t portId_{UINT_MAX};

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
  std::unique_ptr<folly::AsyncTimeout> nlMessageTimer_{nullptr};

  // Timer for initializing this socket. This gets cancelled automatically if
  // event-base is never started
  std::unique_ptr<folly::AsyncTimeout> nlInitTimer_{nullptr};
};

} // namespace openr::fbnl
