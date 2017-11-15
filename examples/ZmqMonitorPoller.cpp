/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ZmqMonitorPoller.h"

#include <glog/logging.h>

#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>

namespace openr {

ZmqMonitorPoller::ZmqMonitorPoller(
  std::vector<fbzmq::SocketUrl>& counterUrls,
  std::vector<fbzmq::SocketUrl>& logSubscriberUrls,
  fbzmq::Context& zmqContext)
: counterUrls_(counterUrls),
  zmqContext_(zmqContext),
  zmqSubscriber_(zmqContext) {
  // set up listener for log publications
  for (const auto& url : logSubscriberUrls) {
    auto rc = zmqSubscriber_.connect(url);
    if (rc.hasError()) {
      LOG(FATAL) << "Error connecting to zmqMonitor publisher endpoint: "
                 << static_cast<std::string>(url) << ". " << rc.error();
    }
  }
  // subscribe
  zmqSubscriber_.setSockOpt(ZMQ_SUBSCRIBE, "", 0).value();
  addSocket(
    fbzmq::RawZmqSocketPtr{*zmqSubscriber_},
    ZMQ_POLLIN,
    [this](int /* revents */) noexcept {
      recvPublication();
    }
  );

  // set periodic timer to collect counters
  periodicCounterTimeout_ =
    fbzmq::ZmqTimeout::make(this, [this]() noexcept {
      for (const auto& url: counterUrls_) {
        getZmqMonitorCounters(url);
      }
    });

  periodicCounterTimeout_->scheduleTimeout(
    std::chrono::seconds(60), true /* isPeriodic */);
}

void
ZmqMonitorPoller::recvPublication() {
  auto maybePub =
    zmqSubscriber_.recvThriftObj<fbzmq::thrift::MonitorPub>(
      compactSerializer_);
  if (maybePub.hasError()) {
    LOG(ERROR) << "Failed to recv publication on zmqSubscriber_: "
               << maybePub.error();
    return;
  }
  auto pub = maybePub.value();
  if (pub.pubType == fbzmq::thrift::PubType::EVENT_LOG_PUB) {
    // look what we got
    LOG(INFO) << "Log message recvd for ctaegory: " << pub.eventLogPub.category
              << " samples: ";
    for (const auto& sample : pub.eventLogPub.samples) {
      LOG(INFO) << sample;
    }
  }
}

void
ZmqMonitorPoller::getZmqMonitorCounters(const fbzmq::SocketUrl& zmqUrl) {
  fbzmq::Socket<ZMQ_DEALER, fbzmq::ZMQ_CLIENT> dealerSock(zmqContext_);

  // Connect socket
  dealerSock.connect(zmqUrl);
  LOG(INFO) << "Connecting to ZmqMonitor rep endpoint: "
            << static_cast<std::string>(zmqUrl);

  // Send request
  fbzmq::thrift::MonitorRequest request;
  request.cmd = fbzmq::thrift::MonitorCommand::DUMP_ALL_COUNTER_DATA;
  dealerSock.sendThriftObj(request, compactSerializer_);

  auto maybeResult = dealerSock.recvThriftObj<
    fbzmq::thrift::CounterValuesResponse>(compactSerializer_);
  if (maybeResult.hasError()) {
    LOG(ERROR) << "Failed getting counters from openr. Error: "
               << maybeResult.error();
    return;
  }
  auto& counters = maybeResult.value().counters;
  LOG(INFO) << "Got " << counters.size() << " counters from ZmqMonitor at "
            << static_cast<std::string>(zmqUrl);

  // Look at what wegot
  for (auto const& kv : counters) {
    LOG(INFO) << "key: " << kv.first << ", value: " << kv.second.value;
  }

  return;
}

} // namespace openr
