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
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {

class ZmqMonitorPoller : public fbzmq::ZmqEventLoop {
 public:
  ZmqMonitorPoller(
    std::vector<fbzmq::SocketUrl>& counterUrls,
    std::vector<fbzmq::SocketUrl>& logSubscriberUrls,
    fbzmq::Context& zmqContext);

 private:
  void recvPublication();
  void getZmqMonitorCounters(const fbzmq::SocketUrl& zmqUrl);

  std::vector<fbzmq::SocketUrl> counterUrls_;
  fbzmq::Context& zmqContext_;
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> zmqSubscriber_;
  apache::thrift::CompactSerializer compactSerializer_;
  std::unique_ptr<fbzmq::ZmqTimeout> periodicCounterTimeout_;

}; // class ZmqMonitorPoller
} // namespace openr
