/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/SparkFaker.h>

#include <fmt/format.h>
#include <glog/logging.h>

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
    int dutIfIndex) {
  FakeNeighbor neighbor;
  neighbor.nodeName = nodeName;
  neighbor.ifName = ifName;
  neighbor.ifIndex = ifIndex;
  neighbor.v6Addr = folly::IPAddressV6(v6Addr);
  neighbor.v4Addr = folly::IPAddressV4("0.0.0.0");
  neighbor.dutIfName = dutIfName;
  neighbor.dutIfIndex = dutIfIndex;
  neighbor.state = thrift::SparkNeighState::IDLE;
  neighbor.seqNum = 1;
  neighbor.lastHelloTime = std::chrono::steady_clock::now();
  neighbor.lastHeartbeatTime = std::chrono::steady_clock::now();

  neighbors_.push_back(std::move(neighbor));

  /*
   * Register the interface with the I/O layer
   */
  io_->addInterface(ifName, ifIndex);

  VLOG(1) << "SparkFaker: Added neighbor " << nodeName << " on " << ifName
          << " (ifIndex=" << ifIndex << ") -> DUT " << dutIfName;
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

  VLOG(1) << "SparkFaker: Started with " << neighbors_.size() << " neighbors";
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

  VLOG(1) << "SparkFaker: Stopped";
}

bool
SparkFaker::failNeighbor(const std::string& nodeName) {
  for (auto& neighbor : neighbors_) {
    if (neighbor.nodeName == nodeName) {
      neighbor.failed = true;
      VLOG(1) << "SparkFaker: Failed neighbor " << nodeName
              << " - stopping packet transmission";
      return true;
    }
  }
  VLOG(1) << "SparkFaker: Neighbor " << nodeName << " not found";
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
      VLOG(1) << "SparkFaker: Recovered neighbor " << nodeName
              << " - resuming packet transmission";
      return true;
    }
  }
  VLOG(1) << "SparkFaker: Neighbor " << nodeName << " not found";
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
      VLOG(1) << "SparkFaker: Set ctrlPort for " << nodeName << " to " << port;
      return true;
    }
  }
  VLOG(1) << "SparkFaker: Neighbor " << nodeName << " not found";
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

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "[SPARK-FAKER] HELLO SENT: " << neighbor.nodeName
            << " -> DUT:" << neighbor.dutIfName
            << " state=" << apache::thrift::util::enumNameSafe(neighbor.state)
            << " seqNum=" << (neighbor.seqNum - 1);
  }
}

void
SparkFaker::sendHandshake(FakeNeighbor& neighbor) {
  if (neighbor.dutNodeName.empty()) {
    VLOG(2) << "SparkFaker: Cannot send handshake, DUT node name unknown";
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

  if (VLOG_IS_ON(2)) {
    VLOG(2) << "[SPARK-FAKER] HANDSHAKE SENT: " << neighbor.nodeName
            << " -> DUT:" << neighbor.dutNodeName << " isAdjEstablished="
            << (neighbor.state == thrift::SparkNeighState::ESTABLISHED);
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

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "[SPARK-FAKER] HEARTBEAT SENT: " << neighbor.nodeName
            << " seqNum=" << (neighbor.seqNum - 1);
  }
}

void
SparkFaker::handleDutPacket(
    const std::string& ifName, const thrift::SparkHelloPacket& packet) {
  /*
   * Find the neighbor that corresponds to this DUT interface
   */
  FakeNeighbor* neighbor = nullptr;
  for (auto& n : neighbors_) {
    if (n.dutIfName == ifName) {
      neighbor = &n;
      break;
    }
  }

  if (!neighbor) {
    if (VLOG_IS_ON(2)) {
      VLOG(2) << "[SPARK-FAKER] WARN: Received packet on unknown interface "
              << ifName;
    }
    return;
  }

  /*
   * Process hello message from DUT
   */
  if (packet.helloMsg().has_value()) {
    const auto& hello = packet.helloMsg().value();
    neighbor->dutNodeName = *hello.nodeName();
    neighbor->dutSeqNum = *hello.seqNum();
    neighbor->dutTimestamp = *hello.sentTsInUs();
    stats_.hellosReceived++;

    if (VLOG_IS_ON(2)) {
      VLOG(2)
          << "[SPARK-FAKER] HELLO RECV: DUT:" << neighbor->dutNodeName << " -> "
          << neighbor->nodeName << " seqNum=" << neighbor->dutSeqNum.value()
          << " state=" << apache::thrift::util::enumNameSafe(neighbor->state);
    }

    /*
     * State machine transition based on hello content
     */
    if (neighbor->state == thrift::SparkNeighState::IDLE) {
      neighbor->state = thrift::SparkNeighState::WARM;
      LOG(INFO) << fmt::format(
          "[SPARK-FAKER] STATE: {} IDLE -> WARM (discovered DUT:{})",
          neighbor->nodeName,
          neighbor->dutNodeName);
    }

    /*
     * Check if DUT sees us (our nodeName in their neighborInfos)
     */
    auto it = hello.neighborInfos()->find(neighbor->nodeName);
    if (it != hello.neighborInfos()->end()) {
      if (neighbor->state == thrift::SparkNeighState::WARM) {
        neighbor->state = thrift::SparkNeighState::NEGOTIATE;
        LOG(INFO) << fmt::format(
            "[SPARK-FAKER] STATE: {} WARM -> NEGOTIATE (DUT sees us)",
            neighbor->nodeName);
        sendHandshake(*neighbor);
      }
    }
  }

  /*
   * Process handshake message from DUT
   */
  if (packet.handshakeMsg().has_value()) {
    const auto& handshake = packet.handshakeMsg().value();
    stats_.handshakesReceived++;

    if (VLOG_IS_ON(2)) {
      VLOG(2) << "[SPARK-FAKER] HANDSHAKE RECV: DUT:" << *handshake.nodeName()
              << " -> " << neighbor->nodeName << " neighborNodeName="
              << handshake.neighborNodeName().value_or("none");
    }

    /*
     * Check if handshake is for us
     */
    if (handshake.neighborNodeName().has_value() &&
        handshake.neighborNodeName().value() == neighbor->nodeName) {
      if (neighbor->state == thrift::SparkNeighState::NEGOTIATE) {
        neighbor->state = thrift::SparkNeighState::ESTABLISHED;
        stats_.neighborsEstablished++;
        LOG(INFO) << fmt::format(
            "[SPARK-FAKER] STATE: {} NEGOTIATE -> ESTABLISHED (adjacency up!)",
            neighbor->nodeName);
      }

      /*
       * Respond with handshake (isAdjEstablished=true)
       */
      sendHandshake(*neighbor);
    }
  }

  /*
   * Process heartbeat - just keep track
   */
  if (packet.heartbeatMsg().has_value()) {
    stats_.heartbeatsReceived++;
    if (VLOG_IS_ON(3)) {
      VLOG(3) << "[SPARK-FAKER] HEARTBEAT RECV: DUT -> " << neighbor->nodeName;
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
     * Send hello periodically
     */
    if (now - neighbor.lastHelloTime >= helloInterval_) {
      sendHello(neighbor);
    }
    break;
  }

  case thrift::SparkNeighState::NEGOTIATE: {
    /*
     * Send hello and handshake
     */
    if (now - neighbor.lastHelloTime >= helloInterval_) {
      sendHello(neighbor);
      sendHandshake(neighbor);
    }
    break;
  }

  case thrift::SparkNeighState::ESTABLISHED: {
    /*
     * Send hello periodically and heartbeat more frequently
     */
    if (now - neighbor.lastHelloTime >= helloInterval_) {
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
  VLOG(1) << "SparkFaker: runLoop started";

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

  VLOG(1) << "SparkFaker: runLoop stopped";
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
    VLOG(2) << "SparkFaker: Registered callback for " << neighbor.ifName;
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
     * Find which DUT interface this came from
     */
    handleDutPacket(srcIfName, pkt);

    if (VLOG_IS_ON(3)) {
      VLOG(3) << "[SPARK-FAKER] Processed packet from " << srcIfName << " to "
              << dstIfName;
    }
  } catch (const std::exception& e) {
    stats_.parseErrors++;
    if (VLOG_IS_ON(2)) {
      VLOG(2) << "[SPARK-FAKER] ERROR: Failed to parse packet from "
              << srcIfName << ": " << e.what();
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
