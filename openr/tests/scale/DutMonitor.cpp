/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/DutMonitor.h>

#include <fmt/format.h>
#include <folly/io/async/AsyncSocket.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

namespace openr {

DutMonitor::DutMonitor(const std::string& dutHost, uint16_t dutPort)
    : dutHost_(dutHost),
      dutPort_(dutPort),
      evb_(std::make_unique<folly::EventBase>()) {}

DutMonitor::~DutMonitor() {
  disconnect();
}

bool
DutMonitor::connect() {
  if (connected_) {
    return true;
  }

  try {
    LOG(INFO) << fmt::format(
        "[DUT-MONITOR] Connecting to DUT at {}:{}...", dutHost_, dutPort_);

    auto socket = folly::AsyncSocket::newSocket(
        evb_.get(), dutHost_, dutPort_, 5000 /* connect timeout ms */);

    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(socket));

    client_ = std::make_unique<apache::thrift::Client<thrift::OpenrCtrl>>(
        std::move(channel));

    connected_ = true;
    LOG(INFO) << "[DUT-MONITOR] Connected to DUT successfully";
    return true;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[DUT-MONITOR] ERROR: Failed to connect to DUT: {}", e.what());
    return false;
  }
}

void
DutMonitor::disconnect() {
  if (client_) {
    client_.reset();
  }
  connected_ = false;
}

bool
DutMonitor::isConnected() const {
  return connected_;
}

std::map<std::string, int64_t>
DutMonitor::getRegexCounters(const std::string& regex) {
  if (!connected_) {
    LOG(ERROR) << "[DUT-MONITOR] ERROR: Not connected to DUT";
    return {};
  }

  try {
    std::map<std::string, int64_t> counters;
    client_->sync_getRegexCounters(counters, regex);
    VLOG(1) << fmt::format(
        "[DUT-MONITOR] Got {} counters matching '{}'", counters.size(), regex);
    return counters;
  } catch (const std::exception& e) {
    LOG(ERROR) << fmt::format(
        "[DUT-MONITOR] ERROR: Failed to get regex counters '{}': {}",
        regex,
        e.what());
    return {};
  }
}

} // namespace openr
