/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/fbmeshd/pinger/PeerPinger.h"

#include <chrono>
#include <thread>

#include <folly/Subprocess.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>

DEFINE_int32(ping_interval_s, 600, "peer ping interval");

PeerPinger::PeerPinger(folly::EventBase* evb) : evb_(evb) {
  attachEventBase(evb_);
}

PeerPinger::~PeerPinger() {
  stop();
}

void
PeerPinger::run() {
  LOG(INFO) << "starting PeerPinger loop";
  scheduleTimeout(0);
  evb_->loopForever();
}

void
PeerPinger::stop() {
  LOG(INFO) << "stopping PeerPinger";
  evb_->terminateLoopSoon();
  LOG(INFO) << "PeerPinger got stopped.";
}

void
PeerPinger::addPeer(const folly::MacAddress& peer) {
  VLOG(2) << "  adding peer " << peer;
  peers_.emplace(peer);
}

void
PeerPinger::removePeer(const folly::MacAddress& peer) {
  VLOG(2) << "removing peer " << peer;
  peers_.erase(peer);
}

void
PeerPinger::parsePingOutput(folly::StringPiece line) {
  std::vector<folly::StringPiece> v;
  folly::split(" ", line, v);
  if (v.size() == 8 && v[1] == "bytes") {
    auto pingLatency = v[6].split_step(" ");
  }
}

void
PeerPinger::pingPeer(const folly::MacAddress& peer) {
  std::string cmd = "/usr/sbin/ping6 ";

  folly::IPAddressV6 ipv6(folly::IPAddressV6::LINK_LOCAL, peer);
  cmd = cmd + ipv6.str() + " -i 0.2 -c 10 -n";

  folly::Subprocess proc(cmd, folly::Subprocess::Options().pipeStdout());

  auto callback = folly::Subprocess::readLinesCallback(
      [&](int /*fd*/, folly::StringPiece line) {
        parsePingOutput(line);
        return false;
      });

  proc.communicate(std::ref(callback), [](int, int) { return true; });
  auto rc = proc.wait();

  if (rc.exitStatus() != 0) {
    throw folly::CalledProcessError(rc);
  }
}

void
PeerPinger::timeoutExpired() noexcept {
  std::chrono::duration<int> pingInterval{FLAGS_ping_interval_s};
  if (peers_.size() == 0) {
    VLOG(1) << "no targets to ping.";
    scheduleTimeout(pingInterval);
    return;
  }

  auto start = std::chrono::steady_clock::now();

  for (const auto& peer : peers_) {
    pingPeer(peer);
  }

  // schedule next run for ping
  auto end = std::chrono::steady_clock::now();
  auto diff = std::chrono::duration_cast<std::chrono::seconds>(end - start);
  VLOG(3) << "ping iteration took " << diff.count() << "s.";
  if (diff >= pingInterval) {
    scheduleTimeout(0);
  } else {
    scheduleTimeout(pingInterval - diff);
  }
}
