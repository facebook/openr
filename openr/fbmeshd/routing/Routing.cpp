/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Routing.h"

#include <chrono>
#include <exception>

#include <glog/logging.h>

#include <folly/MacAddress.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/system/ThreadName.h>
#include <openr/nl/NetlinkSocket.h>

using namespace std::chrono_literals;
using namespace openr::fbmeshd;

namespace {
const uint32_t kMaxMetric{0xffffffff};

const auto kMeshHousekeepingInterval{60s};
const auto kMeshPathExpire{60s};
const auto kMinGatewayRedundancy{2};

void
meshPathExpire(
    std::unordered_map<folly::MacAddress, Routing::MeshPath>& paths) {
  for (auto it = paths.begin(); it != paths.end();) {
    const auto& mpath = it->second;
    if (std::chrono::steady_clock::now() > mpath.expTime + kMeshPathExpire) {
      it = paths.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace

Routing::Routing(
    folly::EventBase* evb,
    MetricManager* metricManager,
    folly::MacAddress nodeAddr,
    uint32_t elementTtl)
    : evb_{evb},
      nodeAddr_{nodeAddr},
      elementTtl_{elementTtl},
      metricManager_{metricManager},
      noLongerAGateRANNTimer_{folly::AsyncTimeout::make(
          *evb_, [this]() noexcept { isRoot_ = false; })},
      housekeepingTimer_{folly::AsyncTimeout::make(
          *evb_, [this]() noexcept { doMeshHousekeeping(); })},
      meshPathRootTimer_{folly::AsyncTimeout::make(
          *evb_, [this]() noexcept { doMeshPathRoot(); })} {
  evb_->runInEventBaseThread([this]() { prepare(); });
}

void
Routing::prepare() {
  doMeshPathRoot();
  doMeshHousekeeping();
}

std::unordered_map<folly::MacAddress, Routing::MeshPath>
Routing::getMeshPaths() {
  std::unordered_map<folly::MacAddress, MeshPath> meshPaths;
  evb_->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this, &meshPaths]() { meshPaths = meshPaths_; });
  return meshPaths;
}

/*
 * Misc utility functions
 */

Routing::MeshPath&
Routing::getMeshPath(folly::MacAddress addr) {
  return meshPaths_
      .emplace(
          std::piecewise_construct,
          std::forward_as_tuple(addr),
          std::forward_as_tuple(addr))
      .first->second;
}

/*
 * Timer callbacks
 */

void
Routing::doMeshHousekeeping() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  meshPathExpire(meshPaths_);
  housekeepingTimer_->scheduleTimeout(kMeshHousekeepingInterval);
}

void
Routing::doMeshPathRoot() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  if (isRoot_) {
    txPannFrame(
        folly::MacAddress::BROADCAST,
        nodeAddr_,
        ++sn_,
        0,
        elementTtl_,
        folly::MacAddress::BROADCAST,
        0,
        isGate_,
        true);

    meshPathRootTimer_->scheduleTimeout(rootPannInterval_);
  }
}

/*
 * Transmit path / path discovery
 */

void
Routing::txPannFrame(
    folly::MacAddress da,
    folly::MacAddress origAddr,
    uint64_t origSn,
    uint8_t hopCount,
    uint8_t ttl,
    folly::MacAddress targetAddr,
    uint32_t metric,
    bool isGate,
    bool replyRequested) {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);

  std::string skb;
  VLOG(10) << "sending PANN orig:" << origAddr << " target:" << targetAddr
           << " dst:" << da.toString();
  serializer_.serialize(
      thrift::MeshPathFramePANN{
          apache::thrift::FRAGILE,
          origAddr.u64NBO(),
          origSn,
          hopCount,
          ttl,
          targetAddr.u64NBO(),
          metric,
          isGate,
          replyRequested,
      },
      &skb);

  auto buf = folly::IOBuf::copyBuffer(skb, 1, 0);
  buf->prepend(1);
  *buf->writableData() = static_cast<uint8_t>(MeshPathFrameType::PANN);

  if (sendPacketCallback_) {
    (*sendPacketCallback_)(da, std::move(buf));
  }
}

/*
 * Receive path processing
 */

void
Routing::receivePacket(
    folly::MacAddress sa, std::unique_ptr<folly::IOBuf> data) {
  auto action = static_cast<MeshPathFrameType>(*data->data());
  data->trimStart(1);

  thrift::MeshPathFramePANN pann;
  switch (action) {
  case MeshPathFrameType::PANN:
    serializer_.deserialize(data.get(), pann);
    hwmpPannFrameProcess(sa, pann);
    break;
  default:
    return;
  }
}

bool
Routing::isStationInTopKGates(folly::MacAddress mac) {
  std::vector<std::pair<uint32_t, folly::MacAddress>> ret;

  for (const auto& mpath : meshPaths_) {
    if (!mpath.second.expired() && mpath.second.isGate) {
      ret.emplace_back(mpath.second.metric, mpath.first);
    }
  }

  std::sort(ret.begin(), ret.end());

  const size_t maxNoGates =
      isGate_ ? kMinGatewayRedundancy - 1 : kMinGatewayRedundancy;

  for (size_t i = 0; i < maxNoGates && i < ret.size(); i++) {
    if (ret[i].second == mac) {
      return true;
    }
  }

  return false;
}

void
Routing::hwmpPannFrameProcess(
    folly::MacAddress sa, thrift::MeshPathFramePANN pann) {
  VLOG(8) << folly::sformat("Routing::{}({}, ...)", __func__, sa.toString());

  folly::MacAddress origAddr{folly::MacAddress::fromNBO(pann.origAddr)};
  uint64_t origSn{pann.origSn};
  uint8_t hopCount{pann.hopCount};
  hopCount++;
  uint32_t origMetric{pann.metric};
  uint8_t ttl{pann.ttl};
  folly::MacAddress targetAddr{folly::MacAddress::fromNBO(pann.targetAddr)};

  /*  Ignore our own PANNs */
  if (origAddr == nodeAddr_) {
    return;
  }

  VLOG(10) << "received PANN from " << origAddr << " via neighbour " << sa
           << " target " << targetAddr << " (is_gate=" << pann.isGate << ")";

  const auto stas = metricManager_->getLinkMetrics();

  const auto sta = stas.find(sa);
  if (sta == stas.end()) {
    VLOG(10) << "discarding PANN - sta not found";
    return;
  }

  folly::MacAddress da{targetAddr};
  if (da.isUnicast() && da != nodeAddr_) {
    const auto targetMpathIt{meshPaths_.find(targetAddr)};
    if (targetMpathIt == meshPaths_.end()) {
      VLOG(10) << "discarding PANN - target not found";
      return;
    }
    const auto& targetMpath = targetMpathIt->second;
    if (targetMpath.expired()) {
      VLOG(10) << "discarding PANN - target expired";
      return;
    }
    da = targetMpath.nextHop;
  }

  uint32_t lastHopMetric{sta->second};

  uint32_t newMetric{origMetric + lastHopMetric};
  if (newMetric < origMetric) {
    newMetric = kMaxMetric;
  }

  auto& mpath = getMeshPath(origAddr);

  /*
   * At this point we decide to ignore some PANNs (i.e. return from here):
   *  - We only ignore PANNs for mpaths that are not expired
   *  - We ignore PANNs that have an old serial number
   *  - We also ignore PANNs that are trying to change the nextHop, unless the
   *    new nextHop has a better metric. This rule prevents route flapping when
   *    we occasionally lose a PANN.
   */
  if (!mpath.expired() &&
      (mpath.sn > origSn ||
       (mpath.nextHop != sa && mpath.metric <= newMetric))) {
    VLOG(10) << "discarding PANN - mpath.sn:" << mpath.sn
             << " origSn:" << origSn << " newMetric" << newMetric
             << " mpath.metric" << mpath.metric;
    return;
  }

  const auto topKGatesOldHasOrig = isStationInTopKGates(origAddr);

  if (pann.isGate &&
      std::count_if(
          meshPaths_.begin(),
          meshPaths_.end(),
          [origAddr, newMetric](const auto& mpathPair) {
            const auto& mpath = mpathPair.second;
            return mpath.dst != origAddr && !mpath.expired() && mpath.isGate &&
                mpath.metric <= newMetric;
          }) >= (isGate_ ? kMinGatewayRedundancy - 1 : kMinGatewayRedundancy)) {
    return;
  }

  mpath.sn = origSn;
  mpath.metric = newMetric;
  mpath.nextHop = sa;
  mpath.nextHopMetric = lastHopMetric;
  mpath.hopCount = hopCount;
  mpath.isGate = pann.isGate;
  mpath.expTime = std::chrono::steady_clock::now() + activePathTimeout_;

  if (pann.replyRequested) {
    txPannFrame(
        mpath.nextHop,
        nodeAddr_,
        ++sn_,
        0,
        elementTtl_,
        origAddr,
        0,
        isGate_,
        false);
  }

  if (ttl <= 1) {
    return;
  }
  ttl--;

  const auto topKGatesNewHasOrig = isStationInTopKGates(origAddr);

  if (targetAddr != nodeAddr_ &&
      (!pann.isGate || topKGatesOldHasOrig || topKGatesNewHasOrig)) {
    txPannFrame(
        da,
        origAddr,
        origSn,
        hopCount,
        ttl,
        targetAddr,
        newMetric,
        pann.isGate,
        pann.replyRequested);
  }
}

/*
 * Management / Control functions
 */

bool
Routing::getGatewayStatus() const {
  bool isGate{};
  evb_->runImmediatelyOrRunInEventBaseThreadAndWait(
      [&isGate, this]() { isGate = isGate_; });
  return isGate;
}

void
Routing::setGatewayStatus(bool isGate) {
  evb_->runInEventBaseThread([isGate, this]() {
    if (isGate_ == isGate) {
      return;
    }
    isGate_ = isGate;

    if (isGate) {
      noLongerAGateRANNTimer_->cancelTimeout();
      if (!isRoot_) {
        isRoot_ = true;
        doMeshPathRoot();
      }
    } else {
      noLongerAGateRANNTimer_->scheduleTimeout(activePathTimeout_);
    }
  });
}

std::unordered_map<folly::MacAddress, Routing::MeshPath>
Routing::dumpMpaths() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  std::unordered_map<folly::MacAddress, Routing::MeshPath> mpaths;
  evb_->runImmediatelyOrRunInEventBaseThreadAndWait(
      [this, &mpaths]() { mpaths = meshPaths_; });
  return mpaths;
}

void
Routing::setSendPacketCallback(
    std::function<void(folly::MacAddress, std::unique_ptr<folly::IOBuf>)> cb) {
  if (evb_->isRunning()) {
    evb_->runImmediatelyOrRunInEventBaseThreadAndWait(
        [this, cb = std::move(cb)]() { sendPacketCallback_ = cb; });
  } else {
    sendPacketCallback_ = cb;
  }
}

void
Routing::resetSendPacketCallback() {
  if (evb_->isRunning()) {
    evb_->runImmediatelyOrRunInEventBaseThreadAndWait(
        [this]() { sendPacketCallback_.reset(); });
  } else {
    sendPacketCallback_.reset();
  }
}
