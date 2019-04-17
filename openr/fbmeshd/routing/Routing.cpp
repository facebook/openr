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
#include <folly/system/ThreadName.h>
#include <openr/nl/NetlinkSocket.h>

using namespace std::chrono_literals;
using namespace openr::fbmeshd;

namespace {
const uint32_t kMaxMetric{0xffffffff};

const auto kMeshHousekeepingInterval{60s};
const auto kMeshPathExpire{600s};
const auto kMaxSaneSnDelta{32};
const auto kSyncRoutesInterval{1s};

void
meshPathExpire(
    std::unordered_map<folly::MacAddress, Routing::MeshPath>& paths) {
  for (auto it = paths.begin(); it != paths.end();) {
    const auto& mpath = it->second;
    if ((!(mpath.flags & Routing::MESH_PATH_RESOLVING)) &&
        (!(mpath.flags & Routing::MESH_PATH_FIXED)) &&
        std::chrono::steady_clock::now() > mpath.expTime + kMeshPathExpire) {
      it = paths.erase(it);
    } else {
      ++it;
    }
  }
}

void
meshPathActivate(Routing::MeshPath& mpath) {
  mpath.flags |= Routing::MeshPathFlags::MESH_PATH_ACTIVE |
      Routing::MeshPathFlags::MESH_PATH_RESOLVED;
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
    openr::fbmeshd::Nl80211Handler& nlHandler,
    folly::SocketAddress addr,
    uint32_t elementTtl)
    : nlHandler_{nlHandler},
      socket_{this},
      clientSocket_{this},
      addr_{addr},
      elementTtl_{elementTtl},
      periodicPinger_{this,
                      folly::IPAddressV6{"ff02::1%mesh0"},
                      folly::IPAddressV6{
                          folly::IPAddressV6::LinkLocalTag::LINK_LOCAL,
                          nlHandler_.lookupMeshNetif().maybeMacAddress.value()},
                      1s,
                      "mesh0"},
      netlinkSocket_{&zmqEvl_},
      zmqEvlThread_{[this]() {
        folly::setThreadName("Routing Zmq Evl");
        zmqEvl_.run();
      }},
      syncRoutesTimer_{folly::AsyncTimeout::make(
          *this,
          [this]() noexcept {
            doSyncRoutes();
            syncRoutesTimer_->scheduleTimeout(kSyncRoutesInterval);
          })},
      noLongerAGateRANNTimer_{folly::AsyncTimeout::make(
          *this,
          [this]() noexcept {
            dot11MeshHWMPRootMode_ = RootModeIdentifier::ROOTMODE_NO_ROOT;
          })},
      housekeepingTimer_{folly::AsyncTimeout::make(
          *this, [this]() noexcept { doMeshHousekeeping(); })},
      meshPathTimer_{folly::AsyncTimeout::make(
          *this, [this]() noexcept { doMeshPath(); })},
      meshPathRootTimer_{folly::AsyncTimeout::make(
          *this, [this]() noexcept { doMeshPathRoot(); })} {
  runInEventBaseThread([this]() { prepare(); });

  runInEventBaseThread([this]() { doMeshHousekeeping(); });

  periodicPinger_.scheduleTimeout(1s);
}

void
Routing::prepare() {
  socket_.bind(addr_);
  clientSocket_.bind(folly::SocketAddress("::", 0));
  VLOG(4) << "Server listening on " << socket_.address().describe();

  socket_.addListener(this, this);
  socket_.listen();
  doMeshPathRoot();

  syncRoutesTimer_->scheduleTimeout(kSyncRoutesInterval);
}

/*
 * L3 Routing over HWMP
 */

void
Routing::doSyncRoutes() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);

  auto meshIfIndex = nlHandler_.lookupMeshNetif().maybeIfIndex.value();
  auto meshMacAddress = nlHandler_.lookupMeshNetif().maybeMacAddress.value();

  zmqEvl_.runInEventLoop([this, meshIfIndex, meshMacAddress]() {
    openr::fbnl::NlUnicastRoutes unicastRouteDb;
    openr::fbnl::NlLinkRoutes linkRouteDb;
    std::vector<fbnl::IfAddress> mesh0Addrs;

    const auto kTaygaIfName{"tayga"};
    auto taygaIfIndex = netlinkSocket_.getIfIndex("tayga").get();

    folly::Optional<std::pair<folly::MacAddress, int32_t>> bestRoot;
    bool isCurrentRootStillAlive = false;
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
        if (currentRoot_ && currentRoot_->first == mpath.dst) {
          isCurrentRootStillAlive = true;
        }
        if (!bestRoot || bestRoot->second > mpath.metric) {
          bestRoot = std::make_pair(mpath.dst, mpath.metric);
        }
      }
    }
    if (bestRoot) {
      VLOG(10) << "Best root: " << bestRoot->first
               << " with metric: " << bestRoot->second;
    } else {
      VLOG(10) << "No root found";
    }
    if (currentRoot_ && isCurrentRootStillAlive) {
      if (bestRoot->second * gatewayChangeThresholdFactor_ <
          currentRoot_->second) {
        currentRoot_ = bestRoot;
      }
    } else {
      currentRoot_ = bestRoot;
    }
    if (currentRoot_) {
      VLOG(10) << "Current root: " << currentRoot_->first
               << " with metric: " << currentRoot_->second;
    } else {
      VLOG(10) << "No current root found";
    }

    auto destination = std::make_pair<folly::IPAddress, uint8_t>(
        folly::IPAddressV6{"fd00:ffff::"}, 96);

    if (dot11MeshGateAnnouncementProtocol_) {
      linkRouteDb.emplace(
          std::make_pair(destination, kTaygaIfName),
          fbnl::RouteBuilder{}
              .setDestination(destination)
              .setProtocolId(98)
              .setRouteIfIndex(taygaIfIndex)
              .setRouteIfName(kTaygaIfName)
              .buildLinkRoute());
    } else if (currentRoot_) {
      const auto defaultV4Prefix =
          std::make_pair<folly::IPAddress, uint8_t>(folly::IPAddressV4{}, 0);

      // ip route add default dev tayga mtu 1260 advmss 1220
      // the MTU is set to 1260 because IPv6 default mtu is 1280, and the IPv4->
      // IPv6 conversion increases the packet size by 20 bytes.
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
                                  meshPaths_.at(currentRoot_->first).nextHop})
                              .setIfIndex(meshIfIndex)
                              .build())
              .build());
    }

    destination =
        folly::CIDRNetwork{getTaygaIPV6FromMacAddress(meshMacAddress), 128};
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

    mesh0Addrs.push_back(
        fbnl::IfAddressBuilder{}
            .setPrefix(folly::CIDRNetwork{
                getMesh0IPV6FromMacAddress(meshMacAddress), 64})
            .setIfIndex(meshIfIndex)
            .build());

    netlinkSocket_.syncIfAddress(
        meshIfIndex, mesh0Addrs, AF_INET6, RT_SCOPE_UNIVERSE);
    netlinkSocket_.syncUnicastRoutes(98, std::move(unicastRouteDb)).get();
    netlinkSocket_.syncLinkRoutes(98, std::move(linkRouteDb)).get();
  });
}

/*
 * Misc utility functions
 */

Routing::MeshPath&
Routing::getMeshPath(folly::MacAddress addr) {
  decltype(meshPaths_)::iterator mpathIt = meshPaths_.find(addr);
  if (mpathIt == meshPaths_.end()) {
    mpathIt =
        meshPaths_
            .emplace(
                std::piecewise_construct,
                std::forward_as_tuple(addr),
                std::forward_as_tuple(
                    addr,
                    folly::AsyncTimeout::make(*this, [this, addr]() noexcept {
                      auto& mpath = meshPaths_.at(addr);
                      if (mpath.flags & MESH_PATH_RESOLVED ||
                          (!(mpath.flags & MESH_PATH_RESOLVING))) {
                        mpath.flags &=
                            ~(MESH_PATH_RESOLVING | MESH_PATH_RESOLVED);
                      } else if (
                          mpath.discoveryRetries <
                          dot11MeshHWMPmaxPREQretries_) {
                        ++mpath.discoveryRetries;
                        mpath.discoveryTimeout *= 2;
                        mpath.flags &= ~MESH_PATH_REQ_QUEUED;
                        meshQueuePreq(mpath, 0);
                      } else {
                        mpath.flags &=
                            ~(MESH_PATH_RESOLVING | MESH_PATH_RESOLVED |
                              MESH_PATH_REQ_QUEUED);
                        mpath.expTime = std::chrono::steady_clock::now();
                      }
                    })))
            .first;
  }
  return mpathIt->second;
}

uint32_t
Routing::getAirtimeLinkMetric(const StationInfo& sta) {
  auto rate = sta.expectedThroughput;
  if (rate == 0) {
    return kMaxMetric;
  }

  /* bitrate is in units of 100 Kbps, while we need rate in units of
   * 1Mbps. This will be corrected on txTime computation.
   */
  rate = 1 + ((rate - 1) / 100);
  uint32_t txTime{((1 << 8) + 10 * (8192 << 8) / rate)};
  uint32_t estimatedRetx{((1 << (2 * 8)) / (1 << 8))};
  uint64_t result{(txTime * estimatedRetx) >> (2 * 8)};
  return static_cast<uint32_t>(result);
}

/*
 * Timer callbacks
 */

void
Routing::doMeshHousekeeping() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  meshPathExpire(meshPaths_);
  meshPathExpire(mppPaths_);
  housekeepingTimer_->scheduleTimeout(kMeshHousekeepingInterval);
}

void
Routing::doMeshPath() {
  meshPathStartDiscovery();
}

void
Routing::doMeshPathRoot() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  if (dot11MeshHWMPRootMode_ > RootModeIdentifier::ROOTMODE_ROOT) {
    txRootFrame();

    std::chrono::milliseconds interval;
    if (dot11MeshHWMPRootMode_ == RootModeIdentifier::PROACTIVE_RANN) {
      interval = dot11MeshHWMPRannInterval_;
    } else {
      interval = dot11MeshHWMProotInterval_;
    }
    meshPathRootTimer_->scheduleTimeout(interval);
  }
}

/*
 * Route info get and refresh
 */

uint32_t
Routing::hwmpRouteInfoGet(
    folly::MacAddress sa,
    folly::MacAddress origAddr,
    uint64_t origSn,
    std::chrono::milliseconds origLifetime,
    uint32_t origMetric,
    uint8_t hopCount) {
  const auto& stas = nlHandler_.getStationsInfo();
  const auto sta =
      std::find_if(stas.begin(), stas.end(), [sa](const auto& sta) {
        return sta.macAddress == sa && sta.expectedThroughput != 0;
      });
  if (sta == stas.end()) {
    return 0;
  }
  uint32_t lastHopMetric{getAirtimeLinkMetric(*sta)};

  /* Update and check originator routing info */
  bool freshInfo{true};

  uint32_t newMetric{origMetric + lastHopMetric};
  if (newMetric < origMetric) {
    newMetric = kMaxMetric;
  }
  auto expTime{std::chrono::steady_clock::now() + origLifetime};

  bool process{true};
  if (origAddr == *nlHandler_.lookupMeshNetif().maybeMacAddress) {
    /* This MP is the originator, we are not interested in this
     * frame, except for updating transmitter's path info.
     */
    process = false;
    freshInfo = false;
  } else {
    auto mpathIt{meshPaths_.find(origAddr)};
    if (mpathIt != meshPaths_.end()) {
      auto& mpath{mpathIt->second};
      if (mpath.flags & MESH_PATH_FIXED) {
        freshInfo = false;
      } else if (
          (mpath.flags & MESH_PATH_ACTIVE) &&
          (mpath.flags & MESH_PATH_SN_VALID)) {
        if (mpath.sn > origSn ||
            (mpath.sn == origSn &&
             (mpath.nextHop != sta->macAddress ? newMetric * 10 / 9
                                               : newMetric) >= mpath.metric)) {
          process = false;
          freshInfo = false;
        }
      } else if (!(mpath.flags & MESH_PATH_ACTIVE)) {
        bool haveSn, newerSn, bounced;

        haveSn = mpath.flags & MESH_PATH_SN_VALID;
        newerSn = haveSn && origSn > mpath.sn;
        bounced = haveSn && ((origSn - mpath.sn) > kMaxSaneSnDelta);

        if (!haveSn || newerSn) {
          /* if SN is newer than what we had
           * then we can take it */
          ;
        } else if (bounced) {
          /* if SN is way different than what
           * we had then assume the other side
           * rebooted or restarted */
          ;
        } else {
          process = false;
          freshInfo = false;
        }
      }
    }
    auto& mpath{getMeshPath(origAddr)};

    if (freshInfo) {
      if (mpath.nextHop != sta->macAddress) {
        mpath.pathChangeCount++;
      }
      mpath.nextHop = sta->macAddress;
      mpath.flags |= MESH_PATH_SN_VALID;
      mpath.metric = newMetric;
      mpath.sn = origSn;
      mpath.expTime = std::max(mpath.expTime, expTime);
      mpath.hopCount = hopCount;
      meshPathActivate(mpath);

      /* draft says preqId should be saved to, but there does
       * not seem to be any use for it, skipping by now
       */
    }
  }

  /* Update and check transmitter routing info */
  auto ta{sa};
  if (origAddr == ta) {
    freshInfo = false;
  } else {
    freshInfo = true;

    auto mpathIt{meshPaths_.find(ta)};
    if (mpathIt != meshPaths_.end()) {
      auto& mpath{mpathIt->second};
      if ((mpath.flags & MESH_PATH_FIXED) ||
          ((mpath.flags & MESH_PATH_ACTIVE) &&
           ((mpath.nextHop != sta->macAddress
                 ? lastHopMetric * 10 / 9
                 : lastHopMetric) > mpath.metric))) {
        freshInfo = false;
      }
    }
    auto& mpath{getMeshPath(ta)};

    if (freshInfo) {
      if (mpath.nextHop != sta->macAddress) {
        mpath.pathChangeCount++;
      }
      mpath.nextHop = sta->macAddress;
      mpath.metric = lastHopMetric;
      mpath.expTime = std::max(mpath.expTime, expTime);
      mpath.hopCount = 1;
      meshPathActivate(mpath);
    }
  }

  return process ? newMetric : 0;
}

/*
 * Transmit path / path discovery
 */

void
Routing::txFrame(
    MeshPathFrameType action,
    uint8_t flags,
    folly::MacAddress origAddr,
    uint64_t origSn,
    uint8_t targetFlags,
    folly::MacAddress targetAddr,
    uint64_t targetSn,
    folly::MacAddress da,
    uint8_t hopCount,
    uint8_t ttl,
    std::chrono::milliseconds lifetime,
    uint32_t metric,
    uint32_t preqId) {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  const auto destSockAddr = folly::SocketAddress{
      da.isBroadcast()
          ? folly::IPAddressV6{"ff02::1%mesh0"}
          : folly::IPAddressV6{folly::IPAddressV6{
                                   folly::IPAddressV6::LINK_LOCAL, da}
                                   .str() +
                               "%mesh0"},
      6668};
  std::string skb;

  switch (action) {
  case MeshPathFrameType::PREQ:
    VLOG(10) << "sending PREQ to " << targetAddr
             << " dst:" << destSockAddr.describe()
             << " origAddr:" << origAddr.toString();
    serializer_.serialize(
        thrift::MeshPathFramePREQ{
            apache::thrift::FRAGILE,
            static_cast<int8_t>(flags),
            static_cast<int8_t>(hopCount),
            static_cast<int8_t>(ttl),
            static_cast<int32_t>(preqId),
            static_cast<int64_t>(origAddr.u64NBO()),
            static_cast<int64_t>(origSn),
            static_cast<int32_t>(lifetime.count()),
            static_cast<int8_t>(metric),
            1,
            static_cast<int8_t>(targetFlags),
            static_cast<int64_t>(targetAddr.u64NBO()),
            static_cast<int64_t>(targetSn),
        },
        &skb);
    break;
  case MeshPathFrameType::PREP:
    VLOG(10) << "sending PREP to " << origAddr
             << " dst:" << destSockAddr.describe()
             << " targetAddr:" << targetAddr.toString();
    serializer_.serialize(
        thrift::MeshPathFramePREP{
            apache::thrift::FRAGILE,
            static_cast<int8_t>(flags),
            static_cast<int8_t>(hopCount),
            static_cast<int8_t>(ttl),
            static_cast<int64_t>(targetAddr.u64NBO()),
            static_cast<int64_t>(targetSn),
            static_cast<int32_t>(lifetime.count()),
            static_cast<int8_t>(metric),
            static_cast<int64_t>(origAddr.u64NBO()),
            static_cast<int64_t>(origSn),
        },
        &skb);
    break;
  case MeshPathFrameType::RANN:
    VLOG(10) << "sending RANN to " << origAddr
             << " dst:" << destSockAddr.describe();
    serializer_.serialize(
        thrift::MeshPathFrameRANN{
            apache::thrift::FRAGILE,
            static_cast<int8_t>(flags),
            static_cast<int8_t>(hopCount),
            static_cast<int8_t>(ttl),
            static_cast<int64_t>(origAddr.u64NBO()),
            static_cast<int64_t>(origSn),
            static_cast<int32_t>(lifetime.count()),
            static_cast<int8_t>(metric),
        },
        &skb);
    break;
  default:
    return;
  }

  auto buf = folly::IOBuf::copyBuffer(skb, 1, 0);
  buf->prepend(1);
  *buf->writableData() = static_cast<uint8_t>(action);
  clientSocket_.write(destSockAddr, buf);
}

void
Routing::txRootFrame() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  auto interval = dot11MeshHWMPRannInterval_;
  uint8_t flags;

  flags = dot11MeshGateAnnouncementProtocol_ ? RANN_FLAG_IS_GATE : 0;

  switch (dot11MeshHWMPRootMode_) {
  case RootModeIdentifier::PROACTIVE_RANN:
    txFrame(
        MeshPathFrameType::RANN,
        flags,
        *nlHandler_.lookupMeshNetif().maybeMacAddress,
        ++sn_,
        0,
        folly::MacAddress{},
        0,
        folly::MacAddress::BROADCAST,
        0,
        elementTtl_,
        interval,
        0,
        0);
    break;
  default:
    LOG(ERROR) << "Proactive mechanism not supported";
    return;
  }
}

void
Routing::meshQueuePreq(
    MeshPath& mpath, std::underlying_type_t<PreqQueueFlags> flags) {
  VLOG(8) << folly::sformat(
      "Routing::{}({}, {})",
      __func__,
      mpath.dst.toString(),
      static_cast<uint32_t>(flags));
  if (mpath.flags & MESH_PATH_REQ_QUEUED) {
    return;
  }

  mpath.flags |= MESH_PATH_REQ_QUEUED;

  preqQueue_.emplace(mpath.dst, flags);

  if (std::chrono::steady_clock::now() >
      lastPreq_ + dot11MeshHWMPpreqMinInterval_) {
    runInEventBaseThreadAlwaysEnqueue([this]() { meshPathStartDiscovery(); });
  } else if (std::chrono::steady_clock::now() < lastPreq_) {
    /* avoid long wait if did not send preqs for a long time
     * and jiffies wrapped around
     */
    lastPreq_ = std::chrono::steady_clock::now() -
        dot11MeshHWMPpreqMinInterval_ - std::chrono::milliseconds{1};
    runInEventBaseThreadAlwaysEnqueue([this]() { meshPathStartDiscovery(); });
  } else {
    meshPathTimer_->scheduleTimeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastPreq_) +
        dot11MeshHWMPpreqMinInterval_);
  }
}

void
Routing::meshPathStartDiscovery() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  if (preqQueue_.size() == 0 ||
      std::chrono::steady_clock::now() <
          lastPreq_ + dot11MeshHWMPpreqMinInterval_) {
    return;
  }

  auto preqNode{preqQueue_.front()};
  preqQueue_.pop();

  auto mpathIt{meshPaths_.find(preqNode.first)};
  if (mpathIt == meshPaths_.end()) {
    return;
  }
  auto& mpath{mpathIt->second};

  if (mpath.flags & (MESH_PATH_DELETED | MESH_PATH_FIXED)) {
    return;
  }
  mpath.flags &= ~MESH_PATH_REQ_QUEUED;
  if (preqNode.second & PREQ_Q_F_START) {
    if (mpath.flags & MESH_PATH_RESOLVING) {
      return;
    } else {
      mpath.flags &= ~MESH_PATH_RESOLVED;
      mpath.flags |= MESH_PATH_RESOLVING;
      mpath.discoveryRetries = 0;
      mpath.discoveryTimeout = minDiscoveryTimeout_;
    }
  } else if (
      !(mpath.flags & MESH_PATH_RESOLVING) ||
      mpath.flags & MESH_PATH_RESOLVED) {
    mpath.flags &= ~MESH_PATH_RESOLVING;
    return;
  }

  lastPreq_ = std::chrono::steady_clock::now();

  if (std::chrono::steady_clock::now() >
          lastSnUpdate_ + dot11MeshHWMPnetDiameterTraversalTime_ ||
      std::chrono::steady_clock::now() < lastSnUpdate_) {
    ++sn_;
    lastSnUpdate_ = std::chrono::steady_clock::now();
  }
  auto lifetime{dot11MeshHWMPactivePathTimeout_};
  uint8_t ttl{static_cast<uint8_t>(elementTtl_)};
  if (ttl == 0) {
    return;
  }

  uint8_t targetFlags{0};
  if (preqNode.second & PREQ_Q_F_REFRESH) {
    targetFlags |= PreqTargetFlags::PREQ_TO_FLAG;
  } else {
    targetFlags &= ~PreqTargetFlags::PREQ_TO_FLAG;
  }

  auto da{(mpath.isRoot) ? mpath.rannSndAddr : folly::MacAddress::BROADCAST};
  txFrame(
      MeshPathFrameType::PREQ,
      0,
      *nlHandler_.lookupMeshNetif().maybeMacAddress,
      sn_,
      targetFlags,
      mpath.dst,
      mpath.sn,
      da,
      0,
      ttl,
      lifetime,
      0,
      preqId_++);
  mpath.timer->scheduleTimeout(mpath.discoveryTimeout);
}

/*
 * Receive path processing
 */

void
Routing::onDataAvailable(
    std::shared_ptr<folly::AsyncUDPSocket> /* socket */,
    const folly::SocketAddress& client,
    std::unique_ptr<folly::IOBuf> data,
    bool truncated) noexcept {
  auto action = static_cast<MeshPathFrameType>(*data->data());
  data->trimStart(1);

  thrift::MeshPathFramePREQ preq;
  thrift::MeshPathFramePREP prep;
  thrift::MeshPathFrameRANN rann;
  uint32_t pathMetric;
  switch (action) {
  case MeshPathFrameType::PREQ:
    serializer_.deserialize(data.get(), preq);
    pathMetric = hwmpRouteInfoGet(
        *client.getIPAddress().asV6().getMacAddressFromLinkLocal(),
        folly::MacAddress::fromNBO(static_cast<uint64_t>(preq.origAddr)),
        static_cast<uint64_t>(preq.origSn),
        std::chrono::milliseconds{static_cast<uint32_t>(preq.lifetime)},
        static_cast<uint32_t>(preq.metric),
        static_cast<uint8_t>(preq.hopCount + 1));
    if (pathMetric != 0) {
      hwmpPreqFrameProcess(
          *client.getIPAddress().asV6().getMacAddressFromLinkLocal(),
          preq,
          pathMetric);
    }
    break;
  case MeshPathFrameType::PREP:
    serializer_.deserialize(data.get(), prep);
    pathMetric = hwmpRouteInfoGet(
        *client.getIPAddress().asV6().getMacAddressFromLinkLocal(),
        folly::MacAddress::fromNBO(static_cast<uint64_t>(prep.targetAddr)),
        static_cast<uint64_t>(prep.targetSn),
        std::chrono::milliseconds{static_cast<uint32_t>(prep.lifetime)},
        static_cast<uint32_t>(prep.metric),
        static_cast<uint8_t>(prep.hopCount + 1));
    if (pathMetric != 0) {
      hwmpPrepFrameProcess(
          *client.getIPAddress().asV6().getMacAddressFromLinkLocal(),
          prep,
          pathMetric);
    }
    break;
  case MeshPathFrameType::RANN:
    serializer_.deserialize(data.get(), rann);
    hwmpRannFrameProcess(
        *client.getIPAddress().asV6().getMacAddressFromLinkLocal(), rann);
    break;
  default:
    return;
  }
}

void
Routing::hwmpPreqFrameProcess(
    folly::MacAddress sa, thrift::MeshPathFramePREQ preq, uint32_t origMetric) {
  VLOG(8) << folly::sformat(
      "Routing::{}({}, ..., {})", __func__, sa.toString(), origMetric);
  /* Update target SN, if present */
  auto targetAddr{
      folly::MacAddress::fromNBO(static_cast<uint64_t>(preq.targetAddr))};
  auto origAddr{
      folly::MacAddress::fromNBO(static_cast<uint64_t>(preq.origAddr))};
  uint64_t targetSn{static_cast<uint64_t>(preq.targetSn)};
  uint64_t origSn{static_cast<uint64_t>(preq.origSn)};
  uint8_t targetFlags{static_cast<uint8_t>(preq.targetFlags)};
  /* Proactive PREQ gate announcements */
  uint8_t flags{static_cast<uint8_t>(preq.flags)};
  bool rootIsGate{!!(flags & RANN_FLAG_IS_GATE)};

  VLOG(10) << "received PREQ from " << origAddr;

  bool reply{false};
  bool forward{true};
  uint32_t targetMetric{0};
  decltype(meshPaths_)::iterator mpathIt;
  if (targetAddr == *nlHandler_.lookupMeshNetif().maybeMacAddress) {
    VLOG(10) << "PREQ is for us";
    forward = false;
    reply = true;
    targetMetric = 0;

    if (targetSn > sn_) {
      sn_ = targetSn;
    }

    if (std::chrono::steady_clock::now() >
            lastSnUpdate_ + dot11MeshHWMPnetDiameterTraversalTime_ ||
        std::chrono::steady_clock::now() < lastSnUpdate_) {
      ++sn_;
      lastSnUpdate_ = std::chrono::steady_clock::now();
    }
    targetSn = sn_;
  } else if (
      targetAddr.isBroadcast() &&
      (targetFlags & PreqTargetFlags::PREQ_TO_FLAG)) {
    mpathIt = meshPaths_.find(origAddr);
    if (mpathIt != meshPaths_.end()) {
      auto& mpath{mpathIt->second};
      if (flags & PreqFlags::PREQ_PROACTIVE_PREP_FLAG) {
        reply = true;
        targetAddr = *nlHandler_.lookupMeshNetif().maybeMacAddress;
        targetSn = ++sn_;
        targetMetric = 0;
        lastSnUpdate_ = std::chrono::steady_clock::now();
      }
      mpath.isGate = rootIsGate;
    }
  } else {
    mpathIt = meshPaths_.find(targetAddr);
    if (mpathIt != meshPaths_.end()) {
      auto& mpath{mpathIt->second};
      if ((!(mpath.flags & MESH_PATH_SN_VALID)) || mpath.sn < targetSn) {
        mpath.sn = targetSn;
        mpath.flags |= MeshPathFlags::MESH_PATH_SN_VALID;
      } else if (
          (!(targetFlags & PreqTargetFlags::PREQ_TO_FLAG)) &&
          (mpath.flags & MeshPathFlags::MESH_PATH_ACTIVE)) {
        reply = true;
        targetMetric = mpath.metric;
        targetSn = mpath.sn;
        /* Case E2 of sec 13.10.9.3 IEEE 802.11-2012*/
        targetFlags |= PreqTargetFlags::PREQ_TO_FLAG;
      }
    }
  }

  if (reply) {
    std::chrono::milliseconds lifetime{static_cast<uint32_t>(preq.lifetime)};
    uint8_t ttl{static_cast<uint8_t>(elementTtl_)};
    if (ttl != 0) {
      VLOG(10) << "replying to the PREQ";
      txFrame(
          MeshPathFrameType::PREP,
          0,
          origAddr,
          origSn,
          0,
          targetAddr,
          targetSn,
          sa,
          0,
          ttl,
          lifetime,
          targetMetric,
          0);
    }
  }

  if (forward && dot11MeshForwarding_) {
    std::chrono::milliseconds lifetime{static_cast<uint32_t>(preq.lifetime)};
    uint8_t ttl{static_cast<uint8_t>(preq.ttl)};
    if (ttl <= 1) {
      return;
    }
    VLOG(10) << "forwarding the PREQ from " << origAddr;
    --ttl;

    uint32_t preqId{static_cast<uint32_t>(preq.preqId)};
    uint8_t hopCount{static_cast<uint8_t>(preq.hopCount + 1)};
    folly::MacAddress da{(mpathIt != meshPaths_.end() && mpathIt->second.isRoot)
                             ? mpathIt->second.rannSndAddr
                             : folly::MacAddress::BROADCAST};

    if (flags & PreqFlags::PREQ_PROACTIVE_PREP_FLAG) {
      targetAddr =
          folly::MacAddress::fromNBO(static_cast<uint64_t>(preq.targetAddr));
      targetSn = static_cast<uint64_t>(preq.targetSn);
    }

    txFrame(
        MeshPathFrameType::PREQ,
        flags,
        origAddr,
        origSn,
        targetFlags,
        targetAddr,
        targetSn,
        da,
        hopCount,
        ttl,
        lifetime,
        origMetric,
        preqId);
  }
}

void
Routing::hwmpPrepFrameProcess(
    folly::MacAddress sa, thrift::MeshPathFramePREP prep, uint32_t metric) {
  VLOG(8) << folly::sformat("Routing::{}({}, ...)", __func__, sa.toString());
  auto targetAddr{
      folly::MacAddress::fromNBO(static_cast<uint64_t>(prep.targetAddr))};

  VLOG(10) << "received PREP from " << targetAddr;

  auto origAddr{
      folly::MacAddress::fromNBO(static_cast<uint64_t>(prep.origAddr))};
  if (origAddr == *nlHandler_.lookupMeshNetif().maybeMacAddress) {
    /* destination, no forwarding required */
    return;
  }

  if (!dot11MeshForwarding_) {
    return;
  }

  uint8_t ttl{static_cast<uint8_t>(prep.ttl)};
  if (ttl <= 1) {
    return;
  }

  auto mpathIt = meshPaths_.find(origAddr);
  if (mpathIt == meshPaths_.end()) {
    return;
  }
  auto& mpath{mpathIt->second};

  if (!(mpath.flags & MeshPathFlags::MESH_PATH_ACTIVE)) {
    return;
  }

  const auto nextHop{mpath.nextHop};
  --ttl;

  uint8_t flags{static_cast<uint8_t>(prep.flags)};
  std::chrono::milliseconds lifetime{static_cast<uint32_t>(prep.lifetime)};
  uint8_t hopCount{static_cast<uint8_t>(prep.hopCount + 1)};
  uint64_t targetSn{static_cast<uint64_t>(prep.targetSn)};
  uint64_t origSn{static_cast<uint64_t>(prep.origSn)};

  txFrame(
      MeshPathFrameType::PREP,
      flags,
      origAddr,
      origSn,
      0,
      targetAddr,
      targetSn,
      nextHop,
      hopCount,
      ttl,
      lifetime,
      metric,
      0);
}

void
Routing::hwmpRannFrameProcess(
    folly::MacAddress sa, thrift::MeshPathFrameRANN rann) {
  VLOG(8) << folly::sformat("Routing::{}({}, ...)", __func__, sa.toString());
  uint8_t ttl{static_cast<uint8_t>(rann.ttl)};
  uint8_t flags{static_cast<uint8_t>(rann.flags)};
  bool rootIsGate{!!(flags & RANN_FLAG_IS_GATE)};
  folly::MacAddress origAddr{
      folly::MacAddress::fromNBO(static_cast<uint64_t>(rann.rootAddr))};
  uint64_t origSn{static_cast<uint64_t>(rann.rootSn)};
  std::chrono::milliseconds interval{static_cast<uint32_t>(rann.interval)};
  uint8_t hopCount{static_cast<uint8_t>(rann.hopCount)};
  hopCount++;
  uint32_t origMetric{static_cast<uint32_t>(rann.metric)};

  /*  Ignore our own RANNs */
  if (origAddr == *nlHandler_.lookupMeshNetif().maybeMacAddress) {
    return;
  }

  VLOG(10) << "received RANN from " << origAddr << " via neighbour " << sa
           << " (is_gate=" << rootIsGate << ")";

  const auto& stas = nlHandler_.getStationsInfo();
  const auto sta =
      std::find_if(stas.begin(), stas.end(), [sa](const auto& sta) {
        return sta.macAddress == sa && sta.expectedThroughput != 0;
      });
  if (sta == stas.end()) {
    VLOG(10) << "discarding RANN - sta not found";
    return;
  }
  uint32_t lastHopMetric{getAirtimeLinkMetric(*sta)};

  uint32_t newMetric{origMetric + lastHopMetric};
  if (newMetric < origMetric) {
    newMetric = kMaxMetric;
  }

  auto& mpath = getMeshPath(origAddr);

  if (mpath.sn >= origSn &&
      !(mpath.sn == origSn && newMetric < mpath.rannMetric)) {
    VLOG(10) << "discarding RANN - mpath.sn:" << mpath.sn
             << " origSn:" << origSn << " newMetric" << newMetric
             << " mpath.rannMetric" << mpath.rannMetric;
    return;
  }

  if ((!(mpath.flags & (MESH_PATH_ACTIVE | MESH_PATH_RESOLVING)) ||
       (std::chrono::steady_clock::now() >
            mpath.lastPreqToRoot + dot11MeshHWMPconfirmationInterval_ ||
        std::chrono::steady_clock::now() < mpath.lastPreqToRoot)) &&
      !(mpath.flags & MESH_PATH_FIXED) && (ttl != 0)) {
    VLOG(10) << "time to refresh root mpath " << origAddr;
    meshQueuePreq(mpath, PREQ_Q_F_START | PREQ_Q_F_REFRESH);
    mpath.lastPreqToRoot = std::chrono::steady_clock::now();
  }

  mpath.sn = origSn;
  mpath.rannMetric = newMetric;
  mpath.isRoot = true;
  /* Recording RANNs sender address to send individually
   * addressed PREQs destined for root mesh STA */
  mpath.rannSndAddr = sa;
  mpath.isGate = rootIsGate;

  if (ttl <= 1) {
    return;
  }
  ttl--;

  if (dot11MeshForwarding_) {
    txFrame(
        MeshPathFrameType::RANN,
        flags,
        origAddr,
        origSn,
        0,
        folly::MacAddress{},
        0,
        folly::MacAddress::BROADCAST,
        hopCount,
        ttl,
        interval,
        newMetric,
        0);
  }
}

/*
 * Management / Control functions
 */

void
Routing::setGatewayStatus(bool isGate) {
  runInEventBaseThread([isGate, this]() {
    if (dot11MeshGateAnnouncementProtocol_ == isGate) {
      return;
    }
    dot11MeshGateAnnouncementProtocol_ = isGate;

    if (isGate) {
      noLongerAGateRANNTimer_->cancelTimeout();
      if (dot11MeshHWMPRootMode_ != RootModeIdentifier::PROACTIVE_RANN) {
        dot11MeshHWMPRootMode_ = RootModeIdentifier::PROACTIVE_RANN;
        doMeshPathRoot();
      }
    } else {
      noLongerAGateRANNTimer_->scheduleTimeout(dot11MeshHWMPactivePathTimeout_);
    }
  });
}

std::unordered_map<folly::MacAddress, Routing::MeshPath>
Routing::dumpMpaths() {
  VLOG(8) << folly::sformat("Routing::{}()", __func__);
  std::unordered_map<folly::MacAddress, Routing::MeshPath> mpaths;
  runImmediatelyOrRunInEventBaseThreadAndWait(
      [this, &mpaths]() { mpaths = meshPaths_; });
  return mpaths;
}
