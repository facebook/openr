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
const auto kSyncRoutesInterval{1s};
const auto kMinGatewayRedundancy{2};
const auto kPeriodicPingerInterval{10s};
const auto kMetricManagerInterval{3s};
const auto kMetricManagerEwmaFactorLog2{7};
const auto kMetricManagerHysteresisFactorLog2{2};
const auto kMetricManagerBaseBitrate{60};

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

folly::IPAddressV6
getIPV6FromMacAddress(const char* prefix, folly::MacAddress macAddress) {
  folly::ByteArray16 bytes;
  const auto* macBytes = macAddress.bytes();
  memcpy(&bytes.front(), prefix, 8);
  bytes[8] = uint8_t(macBytes[0] ^ 0x02);
  bytes[9] = macBytes[1];
  bytes[10] = macBytes[2];
  bytes[11] = 0xff;
  bytes[12] = 0xfe;
  bytes[13] = macBytes[3];
  bytes[14] = macBytes[4];
  bytes[15] = macBytes[5];

  return folly::IPAddressV6::fromBinary(bytes);
}

folly::IPAddressV6
getTaygaIPV6FromMacAddress(folly::MacAddress macAddress) {
  return getIPV6FromMacAddress("\xfd\x00\x00\x00\x00\x00\x00\x00", macAddress);
}

folly::IPAddressV6
getMesh0IPV6FromMacAddress(folly::MacAddress macAddress) {
  return getIPV6FromMacAddress("\xfc\x00\x00\x00\x00\x00\x00\x00", macAddress);
}

} // namespace

Routing::Routing(
    folly::EventBase* evb,
    openr::fbmeshd::Nl80211Handler& nlHandler,
    folly::MacAddress nodeAddr,
    uint32_t elementTtl)
    : evb_{evb},
      nlHandler_{nlHandler},
      nodeAddr_{nodeAddr},
      elementTtl_{elementTtl},
      periodicPinger_{
          evb_,
          folly::IPAddressV6{"ff02::1%mesh0"},
          folly::IPAddressV6{folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                             nodeAddr},
          kPeriodicPingerInterval,
          "mesh0"},
      metricManager_{evb_,
                     kMetricManagerInterval,
                     nlHandler_,
                     kMetricManagerEwmaFactorLog2,
                     kMetricManagerHysteresisFactorLog2,
                     kMetricManagerBaseBitrate},
      netlinkSocket_{&zmqEvl_},
      zmqEvlThread_{[this]() {
        folly::setThreadName("Routing Zmq Evl");
        zmqEvl_.run();
      }},
      syncRoutesTimer_{folly::AsyncTimeout::make(
          *evb_,
          [this]() noexcept {
            doSyncRoutes();
            syncRoutesTimer_->scheduleTimeout(kSyncRoutesInterval);
          })},
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

  periodicPinger_.scheduleTimeout(1s);
  metricManager_.scheduleTimeout(kMetricManagerInterval);
  syncRoutesTimer_->scheduleTimeout(kSyncRoutesInterval);
}

/*
 * L3 Routing over HWMP
 */

void
Routing::doSyncRoutes() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);

  auto meshIfIndex = nlHandler_.lookupMeshNetif().maybeIfIndex.value();

  zmqEvl_.runInEventLoop([this, meshIfIndex]() {
    openr::fbnl::NlUnicastRoutes unicastRouteDb;
    openr::fbnl::NlLinkRoutes linkRouteDb;
    std::vector<fbnl::IfAddress> mesh0Addrs;

    const auto kTaygaIfName{"tayga"};
    auto taygaIfIndex = netlinkSocket_.getIfIndex("tayga").get();

    folly::Optional<std::pair<folly::MacAddress, uint32_t>> bestGate;
    bool isCurrentGateStillAlive = false;
    for (const auto& mpathIt : meshPaths_) {
      const auto& mpath = mpathIt.second;

      if (mpath.nextHop == folly::MacAddress::ZERO) {
        continue;
      }

      auto destination = std::make_pair<folly::IPAddress, uint8_t>(
          getTaygaIPV6FromMacAddress(mpath.dst), 128);
      unicastRouteDb.emplace(
          destination,
          fbnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .addNextHop(fbnl::NextHopBuilder{}
                              .setGateway(folly::IPAddressV6{
                                  folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                                  mpath.nextHop})
                              .setIfIndex(meshIfIndex)
                              .build())
              .build());
      destination = std::make_pair<folly::IPAddress, uint8_t>(
          getMesh0IPV6FromMacAddress(mpath.dst), 128);
      unicastRouteDb.emplace(
          destination,
          fbnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .addNextHop(fbnl::NextHopBuilder{}
                              .setGateway(folly::IPAddressV6{
                                  folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                                  mpath.nextHop})
                              .setIfIndex(meshIfIndex)
                              .build())
              .build());

      if (mpath.expTime > std::chrono::steady_clock::now() && mpath.isGate) {
        if (currentGate_ && currentGate_->first == mpath.dst) {
          isCurrentGateStillAlive = true;
        }
        if (!bestGate || bestGate->second > mpath.metric) {
          bestGate = std::make_pair(mpath.dst, mpath.metric);
        }
      }
    }
    if (bestGate) {
      VLOG(10) << "Best gate: " << bestGate->first
               << " with metric: " << bestGate->second;
    } else {
      VLOG(10) << "No gate found";
    }
    if (currentGate_ && isCurrentGateStillAlive) {
      if (bestGate->second < currentGate_->second) {
        currentGate_ = bestGate;
      }
    } else {
      currentGate_ = bestGate;
    }
    if (currentGate_) {
      VLOG(10) << "Current gate: " << currentGate_->first
               << " with metric: " << currentGate_->second;
    } else {
      VLOG(10) << "No current gate found";
    }

    auto destination =
        folly::CIDRNetwork{getTaygaIPV6FromMacAddress(nodeAddr_), 128};
    linkRouteDb.emplace(
        std::make_pair(destination, kTaygaIfName),
        fbnl::RouteBuilder{}
            .setDestination(destination)
            .setProtocolId(98)
            .setRouteIfIndex(taygaIfIndex)
            .setRouteIfName(kTaygaIfName)
            .buildLinkRoute());

    destination = folly::CIDRNetwork{folly::IPAddressV4{"172.16.0.0"}, 16};
    linkRouteDb.emplace(
        std::make_pair(destination, kTaygaIfName),
        fbnl::RouteBuilder{}
            .setDestination(destination)
            .setProtocolId(98)
            .setRouteIfIndex(taygaIfIndex)
            .setRouteIfName(kTaygaIfName)
            .buildLinkRoute());

    mesh0Addrs.push_back(fbnl::IfAddressBuilder{}
                             .setPrefix(folly::CIDRNetwork{
                                 getMesh0IPV6FromMacAddress(nodeAddr_), 64})
                             .setIfIndex(meshIfIndex)
                             .build());

    netlinkSocket_.syncIfAddress(
        meshIfIndex, mesh0Addrs, AF_INET6, RT_SCOPE_UNIVERSE);

    if (isGateBeforeRouteSync_ != isGate_) {
      netlinkSocket_.syncUnicastRoutes(98, std::move(unicastRouteDb)).get();
      netlinkSocket_.syncLinkRoutes(98, std::move(linkRouteDb)).get();
    }

    destination = std::make_pair<folly::IPAddress, uint8_t>(
        folly::IPAddressV6{"fd00:ffff::"}, 96);

    if (isGate_) {
      linkRouteDb.emplace(
          std::make_pair(destination, kTaygaIfName),
          fbnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .setRouteIfIndex(taygaIfIndex)
              .setRouteIfName(kTaygaIfName)
              .buildLinkRoute());
    } else if (currentGate_) {
      const auto defaultV4Prefix =
          std::make_pair<folly::IPAddress, uint8_t>(folly::IPAddressV4{}, 0);

      // ip route add default dev tayga mtu 1500 advmss 1460
      linkRouteDb.emplace(
          std::make_pair(defaultV4Prefix, kTaygaIfName),
          fbnl::RouteBuilder{}
              .setDestination(defaultV4Prefix)
              .setProtocolId(98)
              .setMtu(1500)
              .setAdvMss(1460)
              .setRouteIfIndex(taygaIfIndex)
              .setRouteIfName(kTaygaIfName)
              .buildLinkRoute());

      unicastRouteDb.emplace(
          destination,
          fbnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .addNextHop(fbnl::NextHopBuilder{}
                              .setGateway(folly::IPAddressV6{
                                  folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                                  meshPaths_.at(currentGate_->first).nextHop})
                              .setIfIndex(meshIfIndex)
                              .build())
              .build());
    }
    isGateBeforeRouteSync_ = isGate_;

    netlinkSocket_.syncUnicastRoutes(98, std::move(unicastRouteDb)).get();
    netlinkSocket_.syncLinkRoutes(98, std::move(linkRouteDb)).get();
  });
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

  const auto stas = nlHandler_.getStationsInfo();
  const auto sta =
      std::find_if(stas.begin(), stas.end(), [sa](const auto& sta) {
        return sta.macAddress == sa && sta.expectedThroughput != 0;
      });
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

  uint32_t lastHopMetric{metricManager_.getLinkMetric(*sta)};

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
