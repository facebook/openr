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
#include <openr/nl/NetlinkSocket.h>
#include <openr/nl/NetlinkTypes.h>

namespace openr {

/**
 * This is a utility class to publish link/addr/neighbor events through ZMQ
 * message passing mechanism. Event will be sent over Zmq PUB socket which
 * OpenR modules can subscribe through SUB socket. The subscriber modules is
 * LinkMonitor from Open/R side.
 */
class PlatformPublisher final : public fbnl::NetlinkSocket::EventsHandler {
 public:
  PlatformPublisher(
      //
      // Immutable state initializers
      //
      fbzmq::Context& context,
      const PlatformPublisherUrl& platformPubUrl);

  ~PlatformPublisher() = default;

  // make no-copy
  PlatformPublisher(const PlatformPublisher&) = delete;
  PlatformPublisher& operator=(const PlatformPublisher&) = delete;

  void publishPlatformEvent(const thrift::PlatformEvent& msg) const;

  void publishLinkEvent(const thrift::LinkEntry& link) const;

  void publishAddrEvent(const thrift::AddrEntry& address) const;

  void publishNeighborEvent(const thrift::NeighborEntry& neighbor) const;

  void stop();

 private:

  // Override method for NetlinkSocket link/address/neighbor events
  void linkEventFunc(
      const std::string& ifName,
      const openr::fbnl::Link& linkEntry) override;

  void addrEventFunc(
      const std::string& ifName,
      const openr::fbnl::IfAddress& addrEntry) override;

  void neighborEventFunc(
      const std::string& ifName,
      const openr::fbnl::Neighbor& neighborEntry) override;

  // Publish link events to, e.g., LinkMonitor and Squire
  const std::string platformPubUrl_;

  // publish our own events (link up/down, addr changes, etc)
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> platformPubSock_;

  // used for communicating over thrift/zmq sockets
  apache::thrift::CompactSerializer serializer_;
};

} // namespace openr
