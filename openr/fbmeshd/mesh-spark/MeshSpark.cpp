/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MeshSpark.h"

#include <chrono>
#include <thread>

#include <folly/IPAddressV6.h>
#include <folly/MacAddress.h>

#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/fbmeshd/common/Constants.h>
#include <openr/fbmeshd/common/Util.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>

using namespace openr::fbmeshd;

DEFINE_int32(
    mesh_spark_cmd_port, Constants::kMeshSparkCmdPort, "mesh spark cmd port");

DEFINE_int32(
    mesh_spark_report_port,
    Constants::kMeshSparkReportPort,
    "mesh spark report port");

DEFINE_int32(mesh_spark_sync_period_s, 20, "mesh spark sync period (second)");

DEFINE_int32(
    mesh_spark_neighbor_hold_time_s, 60, "mesh spark neighbor hold time");

DEFINE_string(
    mesh_spark_peer_whitelist,
    "",
    "Comma-separated list of MAC addresses that we will peer with, when using "
    "MeshSpark; empty string disables this and will allow peering with all "
    "stations");

MeshSpark::MeshSpark(
    fbzmq::ZmqEventLoop& zmqLoop,
    Nl80211Handler& nlHandler,
    const std::string& ifName,
    const openr::KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const openr::KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    fbzmq::Context& zmqContext)
    : zmqLoop_{zmqLoop},
      zmqContext_{zmqContext},
      nlHandler_{nlHandler},
      ifName_{ifName},
      syncPeersInterval_(std::chrono::seconds(FLAGS_mesh_spark_sync_period_s)),
      cmdSocket_(zmqContext),
      reportSocket_(
          zmqContext,
          fbzmq::IdentityString{
              openr::Constants::kSparkReportServerId.toString()},
          folly::none,
          fbzmq::NonblockingFlag{true}),
      kvStoreClient_(
          zmqContext,
          &zmqLoop,
          "node1", /* nodeId is used for writing to kvstore. not used herei */
          kvStoreLocalCmdUrl,
          kvStoreLocalPubUrl) {
  syncPeersTimer_ = fbzmq::ZmqTimeout::make(
      &zmqLoop_, [this]() mutable noexcept { syncPeers(); });
  syncPeersTimer_->scheduleTimeout(syncPeersInterval_, true);

  // bind socket for receiving interface updates
  const std::string meshSparkCmdUrl{
      folly::sformat("tcp://*:{}", FLAGS_mesh_spark_cmd_port)};
  const auto cmd = cmdSocket_.bind(fbzmq::SocketUrl{meshSparkCmdUrl});
  if (cmd.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << meshSparkCmdUrl << "' "
               << cmd.error();
  }

  zmqLoop_.addSocket(
      fbzmq::RawZmqSocketPtr{*cmdSocket_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(2) << "MeshSpark: interface Db received";
        processInterfaceDbUpdate();
      });

  // socket for reporting neighbor changes to link monitor
  // enable handover for duplicate identities
  const int handover = 1;
  const auto reportSockOpt =
      reportSocket_.setSockOpt(ZMQ_ROUTER_HANDOVER, &handover, sizeof(int));
  if (reportSockOpt.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_ROUTER_HANDOVER to " << handover << " "
               << reportSockOpt.error();
  }

  const std::string meshSparkReportUrl{
      folly::sformat("tcp://*:{}", FLAGS_mesh_spark_report_port)};
  LOG(INFO) << "binding reportSocket_ to " << meshSparkReportUrl;
  const auto rep = reportSocket_.bind(fbzmq::SocketUrl{meshSparkReportUrl});
  if (rep.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << meshSparkReportUrl << "' "
               << rep.error();
  }
}

void
MeshSpark::processPrefixDbUpdate() {
  VLOG(1) << folly::sformat("MeshSpark::{}()", __func__);
  kvStoreIPs_.clear();

  const std::string keyPrefix = openr::Constants::kPrefixDbMarker.toString();
  const auto maybeKeyMap = kvStoreClient_.dumpAllWithPrefix(keyPrefix);
  if (maybeKeyMap.hasError()) {
    LOG(ERROR) << maybeKeyMap.error().errString;
    return;
  }
  for (const auto& kv : *maybeKeyMap) {
    auto prefixDb =
        fbzmq::util::readThriftObjStr<openr::thrift::PrefixDatabase>(
            kv.second.value.value(), serializer_);
    for (const auto& prefixEntry : prefixDb.prefixEntries) {
      auto prefixStr = prefixEntry.prefix.prefixAddress.addr;
      // store as nexthop's ip address if it's an ipv4 address set by the
      // prefix allocator.
      if (prefixEntry.type == openr::thrift::PrefixType::PREFIX_ALLOCATOR &&
          prefixStr.size() == folly::IPAddressV4::byteCount() &&
          prefixEntry.prefix.prefixLength == 32) {
        auto macStr = nodeNameToMacAddr(prefixDb.thisNodeName);
        if (!macStr.hasValue()) {
          continue;
        }
        folly::ByteArray4 byteArray{static_cast<unsigned char>(prefixStr[0]),
                                    static_cast<unsigned char>(prefixStr[1]),
                                    static_cast<unsigned char>(prefixStr[2]),
                                    static_cast<unsigned char>(prefixStr[3])};

        kvStoreIPs_.emplace(*macStr, folly::IPAddressV4{byteArray});
      }
    }
  }
}

void
MeshSpark::processInterfaceDbUpdate() {
  auto maybeMsg = cmdSocket_.recvThriftObj<openr::thrift::InterfaceDatabase>(
      serializer_, openr::Constants::kReadTimeout);
  if (maybeMsg.hasError()) {
    LOG(ERROR) << "processInterfaceDbUpdate recv failed: " << maybeMsg.error();
    return;
  }
  openr::thrift::SparkIfDbUpdateResult result;
  result.isSuccess = true;
  cmdSocket_.sendThriftObj(result, serializer_);

  // clear existing peers - so in the next syncPeer() call all 11s
  // established peers will be reported to OpenR.
  peers_.clear();
}

openr::thrift::SparkNeighbor
MeshSpark::createOpenrNeighbor(
    folly::MacAddress macAddr, const folly::IPAddressV4& ipv4Addr) {
  return openr::thrift::SparkNeighbor(
      apache::thrift::FRAGILE,
      "0", /* domain */
      macAddrToNodeName(macAddr),
      FLAGS_mesh_spark_neighbor_hold_time_s,
      "", /* DEPRECATED - public key */
      openr::toBinaryAddress(
          folly::IPAddressV6(folly::IPAddressV6::LINK_LOCAL, macAddr)),
      openr::toBinaryAddress(ipv4Addr),
      openr::Constants::kKvStorePubPort,
      openr::Constants::kKvStoreRepPort,
      ifName_);
}

void
MeshSpark::reportToOpenR(
    const openr::thrift::SparkNeighbor& originator,
    const openr::thrift::SparkNeighborEventType& eventType) {
  openr::thrift::SparkNeighborEvent event{
      apache::thrift::FRAGILE,
      eventType,
      ifName_,
      originator,
      1, /* rtt.count() */
      1, /* label */
      false /* support-flood-optimization */};

  auto ret = reportSocket_.sendMultiple(
      fbzmq::Message::from(openr::Constants::kSparkReportClientId.toString())
          .value(),
      fbzmq::Message(),
      fbzmq::Message::fromThriftObj(event, serializer_).value());
  if (ret.hasError()) {
    LOG(ERROR) << "error sending neighbor info to report socket: "
               << ret.error();
  }
}

void
MeshSpark::addNeighbor(
    folly::MacAddress macAddr, const folly::IPAddressV4& ipv4Addr) {
  VLOG(1) << "creating new neighbor " << macAddr;
  auto originator = createOpenrNeighbor(macAddr, ipv4Addr);
  auto eventType = openr::thrift::SparkNeighborEventType::NEIGHBOR_UP;
  reportToOpenR(originator, eventType);
}

void
MeshSpark::removeNeighbor(
    folly::MacAddress macAddr, const folly::IPAddressV4& ipv4Addr) {
  VLOG(1) << "removing neighbor " << macAddr;
  auto originator = createOpenrNeighbor(macAddr, ipv4Addr);
  auto eventType = openr::thrift::SparkNeighborEventType::NEIGHBOR_DOWN;
  reportToOpenR(originator, eventType);
}

void
MeshSpark::filterWhiteListedPeers(std::vector<folly::MacAddress>& peers) {
  if (FLAGS_mesh_spark_peer_whitelist.empty()) {
    return;
  }
  std::vector<folly::MacAddress> whiteListedPeers;
  auto allowedPeers = parseCsvFlag<folly::MacAddress>(
      FLAGS_mesh_spark_peer_whitelist,
      [](std::string str) { return folly::MacAddress{str}; });
  for (const auto& peer : peers) {
    if (std::find(allowedPeers.begin(), allowedPeers.end(), peer) ==
        allowedPeers.end()) {
      VLOG(8) << folly::sformat(
          "MeshSpark peer {} is not in the peering whitelist, ignoring",
          peer.toString());
      continue;
    } else {
      VLOG(8) << folly::sformat(
          "MeshSpark peer {} is in the peering whitelist, continue syncing",
          peer.toString());
      whiteListedPeers.push_back(peer);
    }
  }
  peers = whiteListedPeers;
}

void
MeshSpark::syncPeers() {
  VLOG(1) << folly::sformat("MeshSpark::{}()", __func__);
  processPrefixDbUpdate();

  std::vector<StationInfo> newStations = nlHandler_.getStationsInfo();
  // remove inactive stations, and keep macAddresses of the active ones
  std::vector<folly::MacAddress> activePeers;
  for (const auto& station : newStations) {
    if (station.inactiveTime < Constants::kMaxPeerInactiveTime) {
      activePeers.push_back(station.macAddress);
    }
  }

  // remove peers that are not white-listed
  filterWhiteListedPeers(activePeers);

  // add new peers: if a new peer exists in the kvstore database, get the
  // ipv4 address from there. Otherwise, initialize with the default
  // ipv4 address.
  for (const auto& peer : activePeers) {
    if (peers_.find(peer) == peers_.end()) {
      folly::IPAddressV4 ip;
      auto keyval = kvStoreIPs_.find(peer);
      if (keyval != kvStoreIPs_.end()) {
        VLOG(2) << "updating ip address of new neighbor " << peer << " to "
                << (keyval->second).str();
        ip = keyval->second;
      } else {
        VLOG(2) << "using default ip address for new neighbor " << peer;
      }
      peers_.emplace(peer, ip);
      addNeighbor(peer, ip);
    }
  }

  // remove neighbors that are not in the new set of peers
  std::unordered_set<folly::MacAddress> newPeerSet(
      activePeers.begin(), activePeers.end());
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (newPeerSet.find(it->first) == newPeerSet.end()) {
      removeNeighbor(it->first, it->second);
      it = peers_.erase(it);
    } else {
      it++;
    }
  }

  // process existing peers: If a peer's ipv4 address is different from
  // kvstore's database, update its ipv4 and add it as a new neighbor
  for (auto& peer : peers_) {
    auto keyval = kvStoreIPs_.find(peer.first);
    if (keyval != kvStoreIPs_.end()) {
      VLOG(2) << "keyval found in kvstore: " << keyval->first << ", "
              << (keyval->second).str();
      if (peer.second != keyval->second) {
        VLOG(2) << "updating ipv4 of neighbor " << peer.first << " from "
                << peer.second << " to " << (keyval->second).str();
        peer.second = keyval->second;
        addNeighbor(peer.first, peer.second);
      }
    }
  }
}
