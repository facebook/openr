/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <syslog.h>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <fbzmq/zmq/Zmq.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/Platform_types.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr {

/**
 * This is a utility class to publish link/addr/neighbor events through ZMQ
 * message passing mechanism. Event will be sent over Zmq PUB socket which
 * OpenR modules can subscribe through SUB socket. The subscriber modules is
 * LinkMonitor from Open/R side.
 */
class PlatformPublisher final {
 public:
  PlatformPublisher(
      fbzmq::Context& context,
      const PlatformPublisherUrl& platformPubUrl,
      fbnl::NetlinkProtocolSocket* nlSock);

  ~PlatformPublisher() = default;

  // make no-copy
  PlatformPublisher(const PlatformPublisher&) = delete;
  PlatformPublisher& operator=(const PlatformPublisher&) = delete;

  void stop();

 private:
  // Implementation functions for publishing link and address events
  void publishLinkEvent(const thrift::LinkEntry& link);
  void publishAddrEvent(const thrift::AddrEntry& address);
  void publishPlatformEvent(const thrift::PlatformEvent& msg);

  // Cache of interface index to name. Used for resolving ifIndex
  // on address events
  // NOTE: We're not assuming any lock. It is always invoked in the same
  // event-loop as of NetlinkProtocolSocket via callback mechanism.
  std::unordered_map<int, std::string> ifIndexToName_;

  // publish our own events (link up/down, addr changes, etc)
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> platformPubSock_;

  // used for communicating over thrift/zmq sockets
  apache::thrift::CompactSerializer serializer_;
};

} // namespace openr
