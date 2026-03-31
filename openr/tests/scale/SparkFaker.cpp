/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/SparkFaker.h>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include <folly/io/IOBuf.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/tests/mocks/MockIoProvider.h>
#include <openr/tests/scale/MockSparkIo.h>

namespace openr {

SparkFaker::SparkFaker(std::shared_ptr<SparkIoInterface> io)
    : io_(std::move(io)) {}

SparkFaker::SparkFaker(std::shared_ptr<MockIoProvider> mockIo)
    : io_(std::make_shared<MockSparkIo>(std::move(mockIo))) {}

SparkFaker::~SparkFaker() {
  stop();
}

void
SparkFaker::addNeighbor(
    const std::string& nodeName,
    const std::string& ifName,
    int ifIndex,
    const std::string& v6Addr,
    const std::string& dutIfName,
    int dutIfIndex,
    const std::string& v4Addr) {
  FakeNeighbor neighbor;
  neighbor.nodeName = nodeName;
  neighbor.ifName = ifName;
  neighbor.ifIndex = ifIndex;
  neighbor.v6Addr = folly::IPAddressV6(v6Addr);
  neighbor.v4Addr = folly::IPAddressV4(v4Addr);
  neighbor.dutIfName = dutIfName;
  neighbor.dutIfIndex = dutIfIndex;
  neighbor.state = thrift::SparkNeighState::IDLE;
  neighbor.seqNum = 1;
  neighbor.lastHelloTime = std::chrono::steady_clock::now();
  neighbor.lastHandshakeTime = std::chrono::steady_clock::now();
  neighbor.lastHeartbeatTime = std::chrono::steady_clock::now();

  neighbors_.push_back(std::move(neighbor));

  /*
   * Register the interface with the I/O layer
   */
  io_->addInterface(ifName, ifIndex);

  XLOGF(
      DBG1,
      "SparkFaker: Added neighbor {} on {} (ifIndex={}) -> DUT {}",
      nodeName,
      ifName,
      ifIndex,
      dutIfName);
}

void
SparkFaker::start() {
  if (running_.exchange(true)) {
    return; /* already running */
  }

  /*
   * Register callbacks with I/O layer to receive DUT's packets
   */
  registerCallbacks();

  /*
   * Start the I/O layer (for RealSparkIo, this starts receive threads)
   */
  io_->startReceiving();

  thread_ = std::make_unique<std::thread>([this]() { runLoop(); });

  XLOGF(DBG1, "SparkFaker: Started with {} neighbors", neighbors_.size());
}

void
SparkFaker::stop() {
  if (!running_.exchange(false)) {
    return; /* already stopped */
  }

  /*
   * Stop the I/O layer (for RealSparkIo, this stops receive threads)
   */
  io_->stopReceiving();

  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
  thread_.reset();

  XLOG(DBG1, "SparkFaker: Stopped");
}

bool
SparkFaker::failNeighbor(const std::string& nodeName) {
  for (auto& neighbor : neighbors_) {
    if (neighbor.nodeName == nodeName) {
      neighbor.failed = true;
      XLOGF(
          DBG1,
          "SparkFaker: Failed neighbor {} - stopping packet transmission",
          nodeName);
      return true;
    }
  }
  XLOGF(DBG1, "SparkFaker: Neighbor {} not found", nodeName);
  return false;
}

bool
SparkFaker::recoverNeighbor(const std::string& nodeName) {
  for (auto& neighbor : neighbors_) {
    if (neighbor.nodeName == nodeName) {
      neighbor.failed = false;
      /*
       * Reset state to IDLE so we go through full handshake again
       */
      neighbor.state = thrift::SparkNeighState::IDLE;
      neighbor.dutSeqNum = std::nullopt;
      neighbor.dutTimestamp = std::nullopt;
      XLOGF(
          DBG1,
          "SparkFaker: Recovered neighbor {} - resuming packet transmission",
          nodeName);
      return true;
    }
  }
  XLOGF(DBG1, "SparkFaker: Neighbor {} not found", nodeName);
  return false;
}

std::optional<thrift::SparkNeighState>
SparkFaker::getNeighborState(const std::string& nodeName) const {
  for (const auto& neighbor : neighbors_) {
    if (neighbor.nodeName == nodeName) {
      return neighbor.state;
    }
  }
  return std::nullopt;
}

bool
SparkFaker::setNeighborCtrlPort(const std::string& nodeName, uint16_t port) {
  for (auto& neighbor : neighbors_) {
    if (neighbor.nodeName == nodeName) {
      neighbor.ctrlPort = port;
      XLOGF(DBG1, "SparkFaker: Set ctrlPort for {} to {}", nodeName, port);
      return true;
    }
  }
  XLOGF(DBG1, "SparkFaker: Neighbor {} not found", nodeName);
  return false;
}

thrift::SparkHelloMsg
SparkFaker::buildHelloMsg(FakeNeighbor& neighbor) {
  thrift::SparkHelloMsg helloMsg;
  helloMsg.nodeName() = neighbor.nodeName;
  helloMsg.ifName() = neighbor.ifName;
  helloMsg.seqNum() = neighbor.seqNum++;

  /*
   * Build neighborInfos map - this proves we see the DUT
   * Only include if we've learned the DUT's info
   */
  if (neighbor.dutSeqNum.has_value()) {
    thrift::ReflectedNeighborInfo neighborInfo;
    neighborInfo.seqNum() = neighbor.dutSeqNum.value();
    neighborInfo.lastNbrMsgSentTsInUs() = neighbor.dutTimestamp.value_or(0);
    neighborInfo.lastMyMsgRcvdTsInUs() =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    helloMsg.neighborInfos()[neighbor.dutNodeName] = neighborInfo;
  }

  /*
   * Version info - just the version number (i32)
   */
  helloMsg.version() = Constants::kOpenrVersion;

  /*
   * Solicit response in IDLE/WARM states for faster discovery
   */
  helloMsg.solicitResponse() =
      (neighbor.state == thrift::SparkNeighState::IDLE ||
       neighbor.state == thrift::SparkNeighState::WARM);

  helloMsg.restarting() = false;
  helloMsg.sentTsInUs() =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  return helloMsg;
}

thrift::SparkHandshakeMsg
SparkFaker::buildHandshakeMsg(
    FakeNeighbor& neighbor, const std::string& dutNodeName) {
  thrift::SparkHandshakeMsg handshakeMsg;
  handshakeMsg.nodeName() = neighbor.nodeName;
  handshakeMsg.isAdjEstablished() =
      (neighbor.state == thrift::SparkNeighState::ESTABLISHED);
  handshakeMsg.holdTime() = holdTime_.count();
  handshakeMsg.gracefulRestartTime() = holdTime_.count(); /* same as holdTime */
  handshakeMsg.transportAddressV6() = toBinaryAddress(neighbor.v6Addr);
  handshakeMsg.transportAddressV4() = toBinaryAddress(neighbor.v4Addr);
  handshakeMsg.openrCtrlThriftPort() = neighbor.ctrlPort;
  handshakeMsg.area() = "0"; /* default area */
  handshakeMsg.neighborNodeName() = dutNodeName;

  return handshakeMsg;
}

thrift::SparkHeartbeatMsg
SparkFaker::buildHeartbeatMsg(FakeNeighbor& neighbor) {
  thrift::SparkHeartbeatMsg heartbeatMsg;
  heartbeatMsg.nodeName() = neighbor.nodeName;
  heartbeatMsg.seqNum() = neighbor.seqNum++;
  heartbeatMsg.holdAdjacency() = false;

  return heartbeatMsg;
}

void
SparkFaker::sendHello(FakeNeighbor& neighbor) {
  auto helloMsg = buildHelloMsg(neighbor);

  thrift::SparkHelloPacket pkt;
  pkt.helloMsg() = std::move(helloMsg);

  auto packet = writeThriftObjStr(pkt, serializer_);

  /*
   * Send packet via SparkIoInterface targeting the DUT's interface
   */
  io_->sendPacket(
      neighbor.dutIfIndex,
      folly::IPAddress(neighbor.v6Addr),
      packet,
      std::chrono::milliseconds(0));

  neighbor.lastHelloTime = std::chrono::steady_clock::now();
  stats_.hellosSent++;

  if (XLOG_IS_ON(DBG3)) {
    XLOGF(
        DBG3,
        "[SPARK-FAKER] HELLO SENT: {} -> DUT:{} state={} seqNum={}",
        neighbor.nodeName,
        neighbor.dutIfName,
        apache::thrift::util::enumNameSafe(neighbor.state),
        neighbor.seqNum - 1);
  }
}

void
SparkFaker::sendHandshake(FakeNeighbor& neighbor) {
  if (neighbor.dutNodeName.empty()) {
    XLOG(DBG2, "SparkFaker: Cannot send handshake, DUT node name unknown");
    return;
  }

  auto handshakeMsg = buildHandshakeMsg(neighbor, neighbor.dutNodeName);

  thrift::SparkHelloPacket pkt;
  pkt.handshakeMsg() = std::move(handshakeMsg);

  auto packet = writeThriftObjStr(pkt, serializer_);

  io_->sendPacket(
      neighbor.dutIfIndex,
      folly::IPAddress(neighbor.v6Addr),
      packet,
      std::chrono::milliseconds(0));

  stats_.handshakesSent++;
  neighbor.lastHandshakeTime = std::chrono::steady_clock::now();

  if (XLOG_IS_ON(DBG2)) {
    XLOGF(
        DBG2,
        "[SPARK-FAKER] HANDSHAKE SENT: {} -> DUT:{} isAdjEstablished={}",
        neighbor.nodeName,
        neighbor.dutNodeName,
        (neighbor.state == thrift::SparkNeighState::ESTABLISHED));
  }
}

void
SparkFaker::sendHeartbeat(FakeNeighbor& neighbor) {
  auto heartbeatMsg = buildHeartbeatMsg(neighbor);

  thrift::SparkHelloPacket pkt;
  pkt.heartbeatMsg() = std::move(heartbeatMsg);

  auto packet = writeThriftObjStr(pkt, serializer_);

  io_->sendPacket(
      neighbor.dutIfIndex,
      folly::IPAddress(neighbor.v6Addr),
      packet,
      std::chrono::milliseconds(0));

  neighbor.lastHeartbeatTime = std::chrono::steady_clock::now();
  stats_.heartbeatsSent++;

  if (XLOG_IS_ON(DBG3)) {
    XLOGF(
        DBG3,
        "[SPARK-FAKER] HEARTBEAT SENT: {} seqNum={}",
        neighbor.nodeName,
        neighbor.seqNum - 1);
  }
}

void
SparkFaker::handleDutPacket(
    const std::string& ifName, const thrift::SparkHelloPacket& packet) {
  /*
   * Dispatch to ALL neighbors whose ifName matches.
   * For MockSparkIo: each neighbor has a unique ifName, so this finds one.
   * For RealSparkIo: the receive loop calls each neighbor's callback
   * separately, so ifName matches one specific neighbor per call.
   */
  bool found = false;
  for (auto& n : neighbors_) {
    if (n.ifName != ifName) {
      continue;
    }
    found = true;
    handleDutPacketForNeighbor(n, packet);
  }

  if (!found) {
    if (XLOG_IS_ON(DBG2)) {
      XLOGF(
          DBG2,
          "[SPARK-FAKER] WARN: Received packet on unknown interface {}",
          ifName);
    }
  }
}

void
SparkFaker::handleDutPacketForNeighbor(
    FakeNeighbor& neighbor, const thrift::SparkHelloPacket& packet) {
  /*
   * Process hello message from DUT
   */
  if (packet.helloMsg().has_value()) {
    const auto& hello = packet.helloMsg().value();

    /*
     * Filter self-packets: if the hello's nodeName matches our own name,
     * this is our own multicast being looped back. Drop it.
     */
    if (*hello.nodeName() == neighbor.nodeName) {
      XLOGF(
          DBG3, "[SPARK-FAKER] Dropping self-hello for {}", neighbor.nodeName);
      return;
    }

    neighbor.dutNodeName = *hello.nodeName();
    neighbor.dutSeqNum = *hello.seqNum();
    neighbor.dutTimestamp = *hello.sentTsInUs();
    stats_.hellosReceived++;

    if (XLOG_IS_ON(DBG2)) {
      XLOGF(
          DBG2,
          "[SPARK-FAKER] HELLO RECV: DUT:{} -> {} seqNum={} state={}",
          neighbor.dutNodeName,
          neighbor.nodeName,
          neighbor.dutSeqNum.value(),
          apache::thrift::util::enumNameSafe(neighbor.state));
    }

    /*
     * State machine transition based on hello content
     */
    if (neighbor.state == thrift::SparkNeighState::IDLE) {
      neighbor.state = thrift::SparkNeighState::WARM;
      XLOGF(
          INFO,
          "[SPARK-FAKER] STATE: {} IDLE -> WARM (discovered DUT:{})",
          neighbor.nodeName,
          neighbor.dutNodeName);
    }

    /*
     * Check if DUT sees us (our nodeName in their neighborInfos)
     */
    auto it = hello.neighborInfos()->find(neighbor.nodeName);
    if (it != hello.neighborInfos()->end()) {
      if (neighbor.state == thrift::SparkNeighState::WARM) {
        neighbor.state = thrift::SparkNeighState::NEGOTIATE;
        XLOGF(
            INFO,
            "[SPARK-FAKER] STATE: {} WARM -> NEGOTIATE (DUT sees us)",
            neighbor.nodeName);
        sendHandshake(neighbor);
      }
    }
  }

  /*
   * Process handshake message from DUT
   */
  if (packet.handshakeMsg().has_value()) {
    const auto& handshake = packet.handshakeMsg().value();

    /*
     * Filter self-packets: if the handshake's nodeName matches our own,
     * this is our own multicast being looped back. Drop it.
     */
    if (*handshake.nodeName() == neighbor.nodeName) {
      XLOGF(
          DBG3,
          "[SPARK-FAKER] Dropping self-handshake for {}",
          neighbor.nodeName);
      return;
    }

    stats_.handshakesReceived++;

    if (XLOG_IS_ON(DBG2)) {
      XLOGF(
          DBG2,
          "[SPARK-FAKER] HANDSHAKE RECV: DUT:{} -> {} neighborNodeName={}",
          *handshake.nodeName(),
          neighbor.nodeName,
          handshake.neighborNodeName().value_or("none"));
    }

    /*
     * Check if handshake is for us
     */
    if (handshake.neighborNodeName().has_value() &&
        handshake.neighborNodeName().value() == neighbor.nodeName) {
      if (neighbor.state == thrift::SparkNeighState::NEGOTIATE) {
        /*
         * Transition to ESTABLISHED. Matches the real Spark FSM which
         * only allows NEGOTIATE + HANDSHAKE_RCVD -> ESTABLISHED.
         * The WARM -> NEGOTIATE transition happens via hello (seeing
         * ourselves in DUT's neighborInfos), driven by fast-init
         * hello rate (500ms) so convergence is fast.
         */
        neighbor.state = thrift::SparkNeighState::ESTABLISHED;
        stats_.neighborsEstablished++;
        XLOGF(
            INFO,
            "[SPARK-FAKER] STATE: {} NEGOTIATE -> ESTABLISHED (adjacency up!)",
            neighbor.nodeName);
        sendHandshake(neighbor);
      } else if (
          neighbor.state == thrift::SparkNeighState::ESTABLISHED &&
          !*handshake.isAdjEstablished()) {
        /*
         * DUT is still negotiating. Reply with isAdjEstablished=true
         * so DUT can complete its transition. Matches real Spark behavior
         * (processHandshakeMsg responds to !isAdjEstablished regardless
         * of local state).
         */
        sendHandshake(neighbor);
      } else if (
          neighbor.state == thrift::SparkNeighState::ESTABLISHED &&
          *handshake.isAdjEstablished() && !neighbor.dutEstablished) {
        /*
         * DUT confirms adjacency is established on its side too.
         * Now we can switch to steady-state hello rate.
         */
        neighbor.dutEstablished = true;
        XLOGF(
            INFO,
            "[SPARK-FAKER] DUT confirmed ESTABLISHED for {}",
            neighbor.nodeName);
      }
    }
  }

  /*
   * Process heartbeat - just keep track
   */
  if (packet.heartbeatMsg().has_value()) {
    stats_.heartbeatsReceived++;

    /*
     * Heartbeats only come from ESTABLISHED neighbors, so this
     * confirms the DUT is established on its side.
     */
    if (!neighbor.dutEstablished) {
      neighbor.dutEstablished = true;
      XLOGF(
          INFO,
          "[SPARK-FAKER] DUT confirmed ESTABLISHED for {} (via heartbeat)",
          neighbor.nodeName);
    }

    if (XLOG_IS_ON(DBG3)) {
      XLOGF(DBG3, "[SPARK-FAKER] HEARTBEAT RECV: DUT -> {}", neighbor.nodeName);
    }
  }
}

void
SparkFaker::processNeighbor(FakeNeighbor& neighbor) {
  /*
   * Skip failed neighbors - they don't send any packets
   */
  if (neighbor.failed) {
    return;
  }

  auto now = std::chrono::steady_clock::now();

  switch (neighbor.state) {
  case thrift::SparkNeighState::IDLE:
  case thrift::SparkNeighState::WARM: {
    /*
     * Send hello at fast-init rate for rapid discovery
     */
    if (now - neighbor.lastHelloTime >= fastInitHelloInterval_) {
      sendHello(neighbor);
    }
    break;
  }

  case thrift::SparkNeighState::NEGOTIATE: {
    /*
     * Send hello and handshake on SEPARATE timers to avoid burst
     * rate limiting on the DUT. Real Spark uses independent timers
     * for hello and handshake sends.
     */
    if (now - neighbor.lastHelloTime >= fastInitHelloInterval_) {
      sendHello(neighbor);
    }
    if (now - neighbor.lastHandshakeTime >= fastInitHelloInterval_) {
      sendHandshake(neighbor);
    }
    break;
  }

  case thrift::SparkNeighState::ESTABLISHED: {
    /*
     * Use fast-init hello rate until DUT confirms ESTABLISHED.
     * This ensures the DUT can complete its negotiation even if
     * the first handshake exchange was lost. Once the DUT confirms
     * (via handshake with isAdjEstablished=true or heartbeat),
     * switch to steady-state 20s hello rate.
     */
    auto helloRate =
        neighbor.dutEstablished ? helloInterval_ : fastInitHelloInterval_;
    if (now - neighbor.lastHelloTime >= helloRate) {
      sendHello(neighbor);
    }
    if (now - neighbor.lastHeartbeatTime >= heartbeatInterval_) {
      sendHeartbeat(neighbor);
    }
    break;
  }

  case thrift::SparkNeighState::RESTART:
    /*
     * RESTART state - treat like IDLE, send hellos to re-establish
     */
    if (now - neighbor.lastHelloTime >= helloInterval_) {
      sendHello(neighbor);
    }
    break;

  default:
    break;
  }
}

void
SparkFaker::runLoop() {
  XLOG(DBG1, "SparkFaker: runLoop started");

  while (running_.load()) {
    /*
     * Process each fake neighbor
     */
    for (auto& neighbor : neighbors_) {
      processNeighbor(neighbor);
    }

    /*
     * Sleep briefly to avoid busy-spinning
     */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  XLOG(DBG1, "SparkFaker: runLoop stopped");
}

void
SparkFaker::registerCallbacks() {
  /*
   * For each neighbor, register a callback to receive packets sent
   * from DUT to that neighbor's interface.
   */
  for (const auto& neighbor : neighbors_) {
    io_->registerCallback(
        neighbor.ifName,
        [this](
            const std::string& srcIfName,
            const std::string& dstIfName,
            const folly::IPAddress& srcAddr,
            const std::string& packet) {
          handleRawDutPacket(srcIfName, dstIfName, srcAddr, packet);
        });
    XLOGF(DBG2, "SparkFaker: Registered callback for {}", neighbor.ifName);
  }
}

void
SparkFaker::handleRawDutPacket(
    const std::string& srcIfName,
    const std::string& dstIfName,
    const folly::IPAddress& /* srcAddr */,
    const std::string& packet) {
  /*
   * Parse the raw packet as SparkHelloPacket
   */
  try {
    auto ioBuf = folly::IOBuf::wrapBuffer(packet.data(), packet.size());
    auto pkt = readThriftObj<thrift::SparkHelloPacket>(*ioBuf, serializer_);

    /*
     * Use dstIfName (the real interface we received on) for dispatch,
     * not srcIfName (which is "dut" / unknown for RealSparkIo).
     */
    handleDutPacket(dstIfName, pkt);

    if (XLOG_IS_ON(DBG3)) {
      XLOGF(
          DBG3,
          "[SPARK-FAKER] Processed packet from {} to {}",
          srcIfName,
          dstIfName);
    }
  } catch (const std::exception& e) {
    stats_.parseErrors++;
    if (XLOG_IS_ON(DBG2)) {
      XLOGF(
          DBG2,
          "[SPARK-FAKER] ERROR: Failed to parse packet from {}: {}",
          srcIfName,
          e.what());
    }
  }
}

std::string
SparkFaker::getNeighborStatusReport() const {
  std::string report = "\n=== SparkFaker Neighbor Status ===\n";
  report += fmt::format(
      "{:<20} {:<15} {:<20} {:<10}\n",
      "Neighbor",
      "State",
      "DUT Node",
      "Failed");
  report += std::string(65, '-') + "\n";

  for (const auto& neighbor : neighbors_) {
    report += fmt::format(
        "{:<20} {:<15} {:<20} {:<10}\n",
        neighbor.nodeName,
        apache::thrift::util::enumNameSafe(neighbor.state),
        neighbor.dutNodeName.empty() ? "(unknown)" : neighbor.dutNodeName,
        neighbor.failed ? "YES" : "no");
  }
  return report;
}

std::string
SparkFaker::getStatsReport() const {
  return fmt::format(
      "\n=== SparkFaker Stats ===\n"
      "Hellos:      sent={:<8} recv={}\n"
      "Handshakes:  sent={:<8} recv={}\n"
      "Heartbeats:  sent={:<8} recv={}\n"
      "Parse errors: {}\n"
      "Neighbors established: {} / {}\n",
      stats_.hellosSent.load(),
      stats_.hellosReceived.load(),
      stats_.handshakesSent.load(),
      stats_.handshakesReceived.load(),
      stats_.heartbeatsSent.load(),
      stats_.heartbeatsReceived.load(),
      stats_.parseErrors.load(),
      stats_.neighborsEstablished.load(),
      neighbors_.size());
}

} // namespace openr
