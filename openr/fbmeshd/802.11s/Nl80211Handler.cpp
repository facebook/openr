/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Sections of this file are derived from iw, that has the following copyrights
 * and license: Copyright (c) 2007, 2008    Johannes Berg Copyright (c) 2007
 * Andy Lutomirski Copyright (c) 2007       Mike Kershaw Copyright
 * (c) 2008-2009                            Luis R. Rodriguez
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 */
#include "Nl80211Handler.h"

extern "C" {
#include <authsae/ampe.h>
#include <authsae/evl_ops.h>
#include <authsae/sae.h>
}
#include <linux/if_ether.h>
#include <netlink/genl/ctrl.h> // @manual
#include <netlink/genl/genl.h> // @manual
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <stdexcept>

#include <glog/logging.h>

#include <folly/Format.h>
#include <folly/MacAddress.h>
#include <folly/ScopeGuard.h>
#include <folly/SingletonThreadLocal.h>
#include <folly/Subprocess.h>

#include <openr/fbmeshd/802.11s/AuthsaeCallbackHelpers.h>
#include <openr/fbmeshd/802.11s/AuthsaeConfigHelpers.h>
#include <openr/fbmeshd/802.11s/NetInterface.h>
#include <openr/fbmeshd/common/Constants.h>
#include <openr/fbmeshd/common/Util.h>
#include <openr/fbmeshd/nl/GenericNetlinkCallbackHandle.h>
#include <openr/fbmeshd/nl/GenericNetlinkFamily.h>
#include <openr/fbmeshd/nl/NestedNetlinkAttribute.h>
#include <openr/fbmeshd/nl/TabularNetlinkAttribute.h>

using namespace openr::fbmeshd;

DEFINE_string(mesh_id, "mesh-soma", "Mesh ID");
DEFINE_string(mesh_channel_type, "40", "Mesh channel type, in string format");
DEFINE_int32(mesh_frequency, 5805, "Mesh control frequency");
DEFINE_int32(mesh_center_freq1, 5795, "Mesh center frequency 1");
DEFINE_int32(mesh_center_freq2, 0, "Mesh center frequency 2");

DEFINE_bool(enable_encryption, false, "Whether to use mesh link encryption");
DEFINE_string(
    encryption_password, "", "Password to use for mesh link encryption");
DEFINE_string(
    encryption_sae_groups,
    "19",
    "Comma-separated list of SAE groups to use for mesh link encryption");

DEFINE_int32(
    userspace_peering_verbosity,
    0,
    "Debug verbosity to use for userspace encryption/peering component");

DEFINE_int32(mesh_rssi_threshold, -80, "Minimum RSSI threshold for the mesh");
DEFINE_string(
    mesh_mac_address,
    "",
    "If set, use this MAC address for the mesh interface");
DEFINE_uint32(mesh_ttl, 31, "TTL for mesh frames");
DEFINE_uint32(mesh_ttl_element, 31, "Element TTL for mesh frames");
DEFINE_uint32(
    mesh_max_peer_links, 32, "Maximum number of allowed 11s peer links");
DEFINE_uint32(mesh_hwmp_active_path_timeout, 30000, "HWMP Active path timeout");
DEFINE_uint32(mesh_hwmp_rann_interval, 3000, "HWMP RANN interval");
DEFINE_uint32(mesh_mtu, 1520, "The MTU value to set for the mesh device");

DEFINE_string(
    mesh_init_peering_whitelist,
    "",
    "Comma-separated list of MAC addresses that we will attempt to initiate "
    "peering with from this station; if another station initiates peering, "
    "this flag will not affect it. Empty string disables this and will attempt "
    "to initiate peering with all stations.");

// Nl80211Handler pointer to be used by authsae for callback forwarding
Nl80211Handler* Nl80211Handler::globalNlHandler;

// Mesh group temporal key defined in authsae
extern unsigned char mgtk_tx[KEY_LEN_AES_CCMP];

namespace {

const auto freq_policy_{[]() {
  std::array<nla_policy, NL80211_FREQUENCY_ATTR_MAX + 1> freq_policy_;

  freq_policy_[NL80211_FREQUENCY_ATTR_FREQ].type = NLA_U32;
  freq_policy_[NL80211_FREQUENCY_ATTR_DISABLED].type = NLA_FLAG;
  freq_policy_[NL80211_FREQUENCY_ATTR_NO_IR].type = NLA_FLAG;
  freq_policy_[__NL80211_FREQUENCY_ATTR_NO_IBSS].type = NLA_FLAG;
  freq_policy_[NL80211_FREQUENCY_ATTR_RADAR].type = NLA_FLAG;
  freq_policy_[NL80211_FREQUENCY_ATTR_MAX_TX_POWER].type = NLA_U32;

  return freq_policy_;
}()};

const auto stats_policy_{[]() {
  std::array<nla_policy, NL80211_STA_INFO_MAX + 1> stats_policy_;

  stats_policy_[NL80211_STA_INFO_INACTIVE_TIME].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_RX_BYTES].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_TX_BYTES].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_RX_BYTES64].type = NLA_U64;
  stats_policy_[NL80211_STA_INFO_TX_BYTES64].type = NLA_U64;
  stats_policy_[NL80211_STA_INFO_RX_PACKETS].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_TX_PACKETS].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_BEACON_RX].type = NLA_U64;
  stats_policy_[NL80211_STA_INFO_SIGNAL].type = NLA_U8;
  stats_policy_[NL80211_STA_INFO_T_OFFSET].type = NLA_U64;
  stats_policy_[NL80211_STA_INFO_TX_BITRATE].type = NLA_NESTED;
  stats_policy_[NL80211_STA_INFO_RX_BITRATE].type = NLA_NESTED;
  stats_policy_[NL80211_STA_INFO_LLID].type = NLA_U16;
  stats_policy_[NL80211_STA_INFO_PLID].type = NLA_U16;
  stats_policy_[NL80211_STA_INFO_PLINK_STATE].type = NLA_U8;
  stats_policy_[NL80211_STA_INFO_TX_RETRIES].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_TX_FAILED].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_BEACON_LOSS].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_RX_DROP_MISC].type = NLA_U64;
  stats_policy_[NL80211_STA_INFO_STA_FLAGS].minlen =
      sizeof(nl80211_sta_flag_update);
  stats_policy_[NL80211_STA_INFO_LOCAL_PM].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_PEER_PM].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_NONPEER_PM].type = NLA_U32;
  stats_policy_[NL80211_STA_INFO_CHAIN_SIGNAL].type = NLA_NESTED;
  stats_policy_[NL80211_STA_INFO_CHAIN_SIGNAL_AVG].type = NLA_NESTED;
  stats_policy_[NL80211_STA_INFO_TID_STATS].type = NLA_NESTED;
  stats_policy_[NL80211_STA_INFO_BSS_PARAM].type = NLA_NESTED;

  return stats_policy_;
}()};

const auto mpath_policy_{[]() {
  std::array<nla_policy, NL80211_MPATH_INFO_MAX + 1> mpath_policy_{};

  mpath_policy_[NL80211_MPATH_INFO_FRAME_QLEN].type = NLA_U32;
  mpath_policy_[NL80211_MPATH_INFO_SN].type = NLA_U32;
  mpath_policy_[NL80211_MPATH_INFO_METRIC].type = NLA_U32;
  mpath_policy_[NL80211_MPATH_INFO_EXPTIME].type = NLA_U32;
  mpath_policy_[NL80211_MPATH_INFO_DISCOVERY_TIMEOUT].type = NLA_U32;
  mpath_policy_[NL80211_MPATH_INFO_DISCOVERY_RETRIES].type = NLA_U8;
  mpath_policy_[NL80211_MPATH_INFO_FLAGS].type = NLA_U8;

  return mpath_policy_;
}()};

} // namespace

Nl80211Handler::Nl80211Handler(
    fbzmq::ZmqEventLoop& zmqLoop,
    const std::string& interfaceName,
    bool userspace_mesh_peering)
    : interfaceName_{interfaceName},
      zmqLoop_{zmqLoop},
      userspace_mesh_peering_{userspace_mesh_peering} {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  // We expect Nl80211Handler to be treated as a singleton, and there should not
  // be more than one in operation at one time. Additionally, because authsae
  // currently stores global state when an encrypted mesh exists, we want to
  // enforce that new Nl80211Handlers are not created after authsae has been
  // initialised for an encrypted mesh.
  CHECK_EQ(globalNlHandler, static_cast<void*>(nullptr));

  validateConfiguration();
  printConfiguration();

  initNlSockets();
  // Now that we have connected sockets, we can read interfaces from the kernel
  for (auto& netInterface : populateNetifs()) {
    const int phyIndex = netInterface.phyIndex();
    netInterfaces_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(phyIndex),
        std::forward_as_tuple(std::move(netInterface)));
  }

  applyConfiguration();
}

// Print the mesh configuration if sufficient verbosity is selected
void
Nl80211Handler::printConfiguration() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  VLOG(1)
      << "mesh id: " << FLAGS_mesh_id << ", interface name: " << interfaceName_
      << ", channel type: " << FLAGS_mesh_channel_type
      << ", control frequency: " << FLAGS_mesh_frequency
      << ", center freq1: " << FLAGS_mesh_center_freq1
      << ", center freq2: " << FLAGS_mesh_center_freq2
      << ", encryption enabled: " << FLAGS_enable_encryption
      << ", encryption sae groups: " << FLAGS_encryption_sae_groups
      << ", userspace peering verbosity: " << FLAGS_userspace_peering_verbosity
      << ", rssi threshold: " << FLAGS_mesh_rssi_threshold
      << ", mac address: " << FLAGS_mesh_mac_address
      << ", ttl: " << FLAGS_mesh_ttl
      << ", element ttl: " << FLAGS_mesh_ttl_element
      << ", hwmp active path timeout: " << FLAGS_mesh_hwmp_active_path_timeout
      << ", hwmp rann interval: " << FLAGS_mesh_hwmp_rann_interval
      << ", max peer links: " << FLAGS_mesh_max_peer_links;
}

// Make sure the configuration values provided via gflags are valid, including
// truncating fields where needed
void
Nl80211Handler::validateConfiguration() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  if (FLAGS_mesh_id.length() > MESHD_MAX_SSID_LEN) {
    LOG(ERROR) << folly::sformat(
        "Mesh ID '{}' is too long - mesh ID is length {} but maximum "
        "(excluding null-term) is {}",
        FLAGS_mesh_id,
        FLAGS_mesh_id.length(),
        MESHD_MAX_SSID_LEN);
    throw std::invalid_argument("Mesh ID is too long");
  }

  if (interfaceName_.length() > IFNAMSIZ) {
    LOG(ERROR) << folly::sformat(
        "Interface '{}' for mesh '{}' is too long - interface is length {} but "
        "maximum (excluding null-term) is {}",
        interfaceName_,
        FLAGS_mesh_id,
        interfaceName_.length(),
        IFNAMSIZ);
    throw std::invalid_argument("Interface name is too long");
  }

  if (!FLAGS_mesh_channel_type.compare("160") &&
      !FLAGS_mesh_channel_type.compare("80+80") &&
      !FLAGS_mesh_channel_type.compare("80") &&
      !FLAGS_mesh_channel_type.compare("40") &&
      !FLAGS_mesh_channel_type.compare("20") &&
      !FLAGS_mesh_channel_type.compare("NOHT")) {
    LOG(ERROR) << folly::sformat(
        "Channel type '{}' is not supported; choose from '160', '80+80', '80', "
        "'40', '20', 'NOHT'",
        FLAGS_mesh_channel_type);
    throw std::invalid_argument("Invalid channel type");
  }

  // Allowed values are defined in kernel net/wireless/nl80211.c
  static constexpr uint16_t kMinMaxPeerLinks{0};
  static constexpr uint16_t kMaxMaxPeerLinks{255};
  if (FLAGS_mesh_max_peer_links > kMaxMaxPeerLinks) {
    LOG(ERROR) << folly::sformat(
        "Max peer links is {} but allowed range is [{}, {}]",
        FLAGS_mesh_max_peer_links,
        kMinMaxPeerLinks,
        kMaxMaxPeerLinks);
    throw std::invalid_argument("Max peer links is outside of valid range");
  }

  // Allowed values are defined in kernel net/wireless/nl80211.c
  static constexpr uint8_t kMinTtl{1};
  static constexpr uint8_t kMaxTtl{255};
  if (FLAGS_mesh_ttl < kMinTtl || FLAGS_mesh_ttl > kMaxTtl) {
    LOG(ERROR) << folly::sformat(
        "TTL is {} but allowed range is [{}, {}]",
        FLAGS_mesh_ttl,
        kMinTtl,
        kMaxTtl);
    throw std::invalid_argument("TTL is outside of valid range");
  }

  // Allowed values are defined in kernel net/wireless/nl80211.c
  static constexpr uint8_t kMinTtlElement{1};
  static constexpr uint8_t kMaxTtlElement{255};
  if (FLAGS_mesh_ttl_element < kMinTtlElement ||
      FLAGS_mesh_ttl_element > kMaxTtlElement) {
    LOG(ERROR) << folly::sformat(
        "Element TTL is {} but allowed range is [{}, {}]",
        FLAGS_mesh_ttl_element,
        kMinTtlElement,
        kMaxTtlElement);
    throw std::invalid_argument("Element TTL is outside of valid range");
  }

  if (FLAGS_enable_encryption) {
    if (FLAGS_encryption_password.length() > SAE_MAX_PASSWORD_LEN - 1) {
      LOG(ERROR) << folly::sformat(
          "Password '{}' for mesh '{}' is too long - password is length {} but "
          "maximum (including null-term) is {}",
          FLAGS_encryption_password,
          FLAGS_mesh_id,
          FLAGS_encryption_password.length(),
          SAE_MAX_PASSWORD_LEN);
      throw std::invalid_argument("Password is too long");
    }

    std::vector<int> saeGroups = parseCsvFlag<int>(
        FLAGS_encryption_sae_groups, [](std::string str) { return stoi(str); });
    if (saeGroups.size() > SAE_MAX_EC_GROUPS) {
      LOG(ERROR) << folly::sformat(
          "Too many SAE finite cyclic groups - {} were specified but up to {}"
          " are allowed ",
          saeGroups.size(),
          SAE_MAX_EC_GROUPS);
      throw std::invalid_argument("Too many SAE finite cyclic groups");
    }
  }
}

void
Nl80211Handler::initNlSockets() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  getNl80211MulticastGroups();

  // Subscribe to mlme events (includes mesh)
  nlEventSocket_.addMembership(multicastGroups_.at("mlme"));
  nlEventSocket_.addMembership(multicastGroups_.at("scan"));

  // Sequence checking must be disabled for events
  nlEventSocket_.disableSequenceChecking();

  // Add event socket to event loop
  zmqLoop_.addSocketFd(nlEventSocket_.getFd(), POLLIN, [this](int) noexcept {
    try {
      eventDataReady();
    } catch (std::exception const& e) {
      LOG(ERROR) << "Error processing data on event socket: " << e.what();
      return;
    }
  });
}

// This method is called by the event loop when there is a message ready on the
// event socket. It receives the message and sets up processEvent() as a
// callback for processing.
void
Nl80211Handler::eventDataReady() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  auto& cb = nlEventSocket_.getCallbackHandle();

  cb.setValidCallback(
      [this](const GenericNetlinkMessage& msg) { return processEvent(msg); });

  nlEventSocket_.tryReceive(cb);
}

// Obtain the list of mesh capable radios from the kernel
std::vector<NetInterface>
Nl80211Handler::populateNetifs() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  std::vector<NetInterface> netInterfaces;

  GenericNetlinkSocket{}.sendAndReceive(
      GenericNetlinkMessage{
          GenericNetlinkFamily::NL80211(), NL80211_CMD_GET_WIPHY, NLM_F_DUMP},
      [&netInterfaces](const GenericNetlinkMessage& msg) {
        const auto tb_msg = msg.getAttributes<NL80211_ATTR_MAX>();

        auto phyIndex = nla_get_u32(tb_msg[NL80211_ATTR_WIPHY]);

        if (netInterfaces.size() == 0 ||
            netInterfaces.back().phyIndex() != static_cast<int>(phyIndex)) {
          // This is a new interface in the listing
          netInterfaces.emplace_back(static_cast<int>(phyIndex));
          netInterfaces.back().phyName =
              std::string{nla_get_string(tb_msg.at(NL80211_ATTR_WIPHY_NAME))};
        }

        NetInterface& netInterface = netInterfaces.back();

        if (tb_msg[NL80211_ATTR_SUPPORTED_IFTYPES]) {
          for (const auto& nl_mode :
               NestedNetlinkAttribute{tb_msg[NL80211_ATTR_SUPPORTED_IFTYPES]}) {
            if (nla_type(nl_mode) == NL80211_IFTYPE_MESH_POINT) {
              netInterface.isMeshCapable = true;
            }
          }
        }

        if (tb_msg[NL80211_ATTR_WIPHY_BANDS]) {
          parseWiphyBands(netInterface, tb_msg);
        }

        return NL_OK;
      });

  return netInterfaces;
}

// This function parses wiphy data (frequencies, bands, rates) from a netlink
// message containing NL80211_ATTR_WIPHY_BANDS into the configuration data in a
// given NetInterface
void
Nl80211Handler::parseWiphyBands(
    NetInterface& netInterface,
    const TabularNetlinkAttribute<NL80211_ATTR_MAX>& tb_msg) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {})", __func__, netInterface.phyIndex());

  for (const auto& nl_band :
       NestedNetlinkAttribute{tb_msg[NL80211_ATTR_WIPHY_BANDS]}) {
    // Set up supported band info for authsae-powered secure meshes
    ieee80211_band band = (ieee80211_band)nl_band->nla_type;
    ieee80211_supported_band* supportedBand =
        netInterface.getSupportedBand(band);
    CHECK(supportedBand);

    TabularNetlinkAttribute<NL80211_BAND_ATTR_MAX> tb_band{nl_band};

    if (tb_band[NL80211_BAND_ATTR_HT_MCS_SET]) {
      assert(
          sizeof(supportedBand->ht_cap.mcs) ==
          nla_len(tb_band[NL80211_BAND_ATTR_HT_MCS_SET]));
      supportedBand->ht_cap.ht_supported = true;
      memcpy(
          &supportedBand->ht_cap.mcs,
          nla_data(tb_band[NL80211_BAND_ATTR_HT_MCS_SET]),
          nla_len(tb_band[NL80211_BAND_ATTR_HT_MCS_SET]));
      supportedBand->ht_cap.cap =
          nla_get_u16(tb_band[NL80211_BAND_ATTR_HT_CAPA]);
      supportedBand->ht_cap.ampdu_factor =
          nla_get_u8(tb_band[NL80211_BAND_ATTR_HT_AMPDU_FACTOR]);
      supportedBand->ht_cap.ampdu_density =
          nla_get_u8(tb_band[NL80211_BAND_ATTR_HT_AMPDU_DENSITY]);
    }
    if (tb_band[NL80211_BAND_ATTR_VHT_MCS_SET]) {
      assert(
          sizeof(supportedBand->vht_cap.mcs) ==
          nla_len(tb_band[NL80211_BAND_ATTR_VHT_MCS_SET]));
      supportedBand->vht_cap.vht_supported = true;
      memcpy(
          &supportedBand->vht_cap.mcs,
          nla_data(tb_band[NL80211_BAND_ATTR_VHT_MCS_SET]),
          nla_len(tb_band[NL80211_BAND_ATTR_VHT_MCS_SET]));
      supportedBand->vht_cap.cap =
          nla_get_u32(tb_band[NL80211_BAND_ATTR_VHT_CAPA]);
    }

    // Set up supported rates for authsae-powered secure meshes
    static std::array<nla_policy, NL80211_BITRATE_ATTR_MAX + 1> rate_policy{};
    rate_policy[NL80211_BITRATE_ATTR_RATE].type = NLA_U32;
    rate_policy[NL80211_BITRATE_ATTR_2GHZ_SHORTPREAMBLE].type = NLA_FLAG;

    std::array<uint16_t, MAX_SUPP_RATES> sup_rates{};

    int n_rates = 0;

    for (const auto& nl_rate :
         NestedNetlinkAttribute{tb_band[NL80211_BAND_ATTR_RATES]}) {
      TabularNetlinkAttribute<NL80211_BITRATE_ATTR_MAX> tb_rate{nl_rate,
                                                                rate_policy};
      if (!tb_rate[NL80211_BITRATE_ATTR_RATE]) {
        VLOG(8) << "Found something other than an ATTR_RATE in the set "
                   "of ATTR_RATES; skipping it";
        continue;
      }
      sup_rates[n_rates] = nla_get_u32(tb_rate[NL80211_BITRATE_ATTR_RATE]);
      n_rates++;
    }

    VLOG(8) << folly::sformat("Found {} rates for band {}", n_rates, (int)band);
    if (n_rates) {
      // supportedBand->rates is 16bit
      supportedBand->rates = (uint16_t*)std::realloc(
          supportedBand->rates, n_rates * sizeof(uint16_t));
      CHECK_NOTNULL(supportedBand->rates);
      memcpy(supportedBand->rates, sup_rates.data(), n_rates * 2);
      supportedBand->n_bitrates = n_rates;
    }

    // Set up supported frequencies
    if (tb_band[NL80211_BAND_ATTR_FREQS]) {
      for (const auto& nl_freq :
           NestedNetlinkAttribute{tb_band[NL80211_BAND_ATTR_FREQS]}) {
        uint32_t freq;
        TabularNetlinkAttribute<NL80211_FREQUENCY_ATTR_MAX> tb_freq{
            nl_freq, freq_policy_};
        if (!tb_freq[NL80211_FREQUENCY_ATTR_FREQ]) {
          continue;
        }
        freq = nla_get_u32(tb_freq[NL80211_FREQUENCY_ATTR_FREQ]);
        netInterface.addSupportedFrequency(freq);
      }
    }
  }
}

NetInterface&
Nl80211Handler::lookupNetifFromPhy(int phyIndex) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}(phy: {})", __func__, phyIndex);

  if (netInterfaces_.empty()) {
    throw std::runtime_error("No mesh-capable interface has been discovered");
  }

  try {
    return netInterfaces_.at(phyIndex);
  } catch (const std::out_of_range& oor) {
    throw std::runtime_error("Requested phyIndex not found");
  }
}

// Returns NetInterface object that is being used for meshing (as only one
// interface may be used for meshing at a time)
//
// Note: this is intended for use when a mesh interface already exists; if no
// encrypted interface is found, a std::runtime_error is thrown.
NetInterface&
Nl80211Handler::lookupMeshNetif() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  if (netInterfaces_.empty()) {
    throw std::runtime_error("No mesh-capable interfaces have been discovered");
  }

  for (auto& intf : netInterfaces_) {
    if (intf.second.maybeIfName) {
      return intf.second;
    }
  }

  throw std::runtime_error("No active mesh interface found");
}

// Returns MAC address of the interface that was created
folly::MacAddress
Nl80211Handler::createInterface(int phyIndex) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}(phy: {})", __func__, phyIndex);

  const NetInterface& netif = lookupNetifFromPhy(phyIndex);
  if (!netif.isValid()) {
    throw std::runtime_error(
        "Cannot create interface with incomplete configuration");
  }

  std::array<unsigned char, ETH_ALEN> address;

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_NEW_INTERFACE};
  nla_put_u32(msg, NL80211_ATTR_WIPHY, netif.phyIndex());
  nla_put_string(msg, NL80211_ATTR_IFNAME, netif.maybeIfName->c_str());
  nla_put_u32(msg, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_MESH_POINT);
  if (netif.maybeMacAddress) {
    nla_put(msg, NL80211_ATTR_MAC, ETH_ALEN, netif.maybeMacAddress->bytes());
  }
  GenericNetlinkSocket{}.sendAndReceive(
      msg, [&address](const GenericNetlinkMessage& msg) {
        const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

        if (!tb[NL80211_ATTR_MAC]) {
          LOG(ERROR) << "interface MAC info missing";
          return NL_SKIP;
        }

        memcpy(address.data(), nla_get_string(tb[NL80211_ATTR_MAC]), ETH_ALEN);

        return NL_OK;
      });

  if (FLAGS_mesh_mtu != 1500) {
    folly::Subprocess{std::vector<std::string>{"/sbin/ip",
                                               "link",
                                               "set",
                                               "dev",
                                               *netif.maybeIfName,
                                               "mtu",
                                               std::to_string(FLAGS_mesh_mtu)}}
        .wait();
  }

  return folly::MacAddress::fromBinary({address.data(), address.size()});
}

void
Nl80211Handler::leaveMesh(int phyIndex) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}(phy: {})", __func__, phyIndex);

  const NetInterface& netif = lookupNetifFromPhy(phyIndex);
  CHECK(netif.maybeIfIndex.hasValue());

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_LEAVE_MESH};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);

  VLOG(8) << folly::sformat(
      "Nl80211Handler::leaveMesh({}): "
      "ifindex: {} ",
      phyIndex,
      ifIndex);
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

void
Nl80211Handler::deleteInterface(const std::string& ifName) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}(ifName: {})", __func__, ifName);

  CHECK(!ifName.empty());

  uint32_t ifIndex = if_nametoindex(ifName.c_str());
  if (ifIndex == 0) {
    VLOG(8) << folly::sformat(
        "Interface '{}' was not found, skipping deletion", ifName);
    return;
  } else {
    VLOG(8) << folly::sformat(
        "Deleting interface '{}' (index {})", ifName, ifIndex);
  }

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_DEL_INTERFACE};
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

status_t
Nl80211Handler::initMesh(int phyIndex) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}(phy: {})", __func__, phyIndex);

  NetInterface& netif = lookupNetifFromPhy(phyIndex);
  if (userspace_mesh_peering_ || netif.isEncrypted) {
    VLOG(8) << folly::sformat(
        "Nl80211Handler::{} initializing userspace mesh peering", __func__);

    // At this stage, we are ready to initialise authsae, so we need to prepare
    // the C functions it uses as callbacks. As we do all netlink operations
    // using this class, we keep track of the existing Nl80211Handler and use
    // the required static C functions to forward calls to it.
    //
    // It is possible for initMesh to be called multiple times in the span of
    // one Nl80211Handler (e.g. if a mesh has been joined and then left); as
    // such, we allow the case where the current Nl80211Handler has already been
    // stored as the global one. However, if it is initialised to a different
    // instance, we must crash, since only one encrypted mesh is currently
    // supported due to authsae's global state.
    if (globalNlHandler == nullptr) {
      globalNlHandler = this;
    }
    CHECK_EQ(globalNlHandler, this);

    authsae_sae_config saeConfig = createSaeConfig(netif);
    auto saeCallbacks = getSaeCallbacks();

    VLOG(8)
        << folly::sformat("Nl80211Handler::{} : sae_initialize()", __func__);
    auto returnValue = sae_initialize(
        const_cast<char*>(netif.meshId.c_str()), &saeConfig, saeCallbacks);
    if (returnValue != 1) {
      return ERR_AUTHSAE;
    }

    authsae_mesh_node* mesh = createMeshConfig(netif);
    auto ampeCallbacks = getAmpeCallbacks();

    // TODO: rekeying doesn't currently appear to work, so skipping it for now
    /*
    VLOG(8) << folly::sformat("Nl80211Handler::{} : rekey_init()", __func__);
    rekey_init(srvctx, mesh);
    */

    VLOG(8)
        << folly::sformat("Nl80211Handler::{} : ampe_initialize()", __func__);
    returnValue = ampe_initialize(mesh, ampeCallbacks);
    if (returnValue != 0) {
      LOG(ERROR) << "Failed to configure AMPE";
      return ERR_AUTHSAE;
    }

    registerForAuthFrames(netif);

    registerForMeshPeeringFrames(netif);

    if (mesh->conf->is_secure) {
      if (mesh->conf->pmf) {
        // Key to protect integrity of multicast mgmt frames tx
        installKey(
            netif,
            folly::none,
            Constants::CIPHER_AES_CMAC,
            NL80211_KEYTYPE_GROUP,
            mesh->igtk_keyid,
            mesh->igtk_tx);
      }

      // Key to encrypt multicast data traffic
      installKey(
          netif,
          folly::none,
          Constants::CIPHER_CCMP,
          NL80211_KEYTYPE_GROUP,
          1,
          mgtk_tx);
    }
  } else {
    VLOG(8) << folly::sformat(
        "Nl80211Handler::{} no initialization needed for kernel mesh peering",
        __func__);
  }

  return R_SUCCESS;
}

void
Nl80211Handler::joinMesh(int phyIndex) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}(phy: {})", __func__, phyIndex);

  const NetInterface& netif = lookupNetifFromPhy(phyIndex);
  CHECK(netif.maybeIfIndex.hasValue());

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_JOIN_MESH};
  nla_put(
      msg, NL80211_ATTR_MESH_ID, netif.meshId.length(), netif.meshId.c_str());
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();

  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  nla_put_u32(msg, NL80211_ATTR_WIPHY_FREQ, netif.frequency);
  nla_put_u32(msg, NL80211_ATTR_CHANNEL_WIDTH, netif.channelWidth);
  nla_put_u32(msg, NL80211_ATTR_CENTER_FREQ1, netif.centerFreq1);
  nla_put_u32(msg, NL80211_ATTR_CENTER_FREQ2, netif.centerFreq2);

  VLOG(8) << folly::sformat(
      "Nl80211Handler::joinMesh(phy: {}): "
      "encrypted: {} "
      "mesh_id: {} "
      "ifindex: {} "
      "wiphy_freq: {} "
      "channel_width: {} "
      "center_freq1: {} "
      "center_freq2: {} ",
      phyIndex,
      netif.isEncrypted,
      netif.meshId.c_str(),
      ifIndex,
      netif.frequency,
      netif.channelWidth,
      netif.centerFreq1,
      netif.centerFreq2);

  nlattr* container = nla_nest_start(msg, NL80211_ATTR_MESH_PARAMS);
  CHECK_NOTNULL(container);

  if (netif.maybeRssiThreshold) {
    uint32_t rssiTh = netif.maybeRssiThreshold.value();
    nla_put_u32(msg, NL80211_MESHCONF_RSSI_THRESHOLD, rssiTh);
  }

  nla_put_u16(msg, NL80211_MESHCONF_MAX_PEER_LINKS, netif.maxPeerLinks);

  nla_put_u8(msg, NL80211_MESHCONF_TTL, netif.ttl);
  nla_put_u8(msg, NL80211_MESHCONF_ELEMENT_TTL, netif.elementTtl);
  nla_put_u32(
      msg,
      NL80211_MESHCONF_HWMP_ACTIVE_PATH_TIMEOUT,
      netif.hwmpActivePathTimeout);
  nla_put_u32(msg, NL80211_MESHCONF_HWMP_RANN_INTERVAL, netif.hwmpRannInterval);

  nla_nest_end(msg, container);

  // If we're handling mesh peering ourselves (in userspace), we need to
  // specify additional info. Note that all encrypted mesh peering is done in
  // userspace.
  if (userspace_mesh_peering_ || netif.isEncrypted) {
    container = nla_nest_start(msg, NL80211_ATTR_MESH_SETUP);
    CHECK_NOTNULL(container);

    // We'll be creating stations and handling peering, not the kernel
    nla_put_flag(msg, NL80211_MESH_SETUP_USERSPACE_MPM);

    if (netif.isEncrypted) {
      // Tell the kernel we're using SAE/AMPE
      nla_put_flag(msg, NL80211_MESH_SETUP_USERSPACE_AUTH);
      nla_put_flag(msg, NL80211_MESH_SETUP_USERSPACE_AMPE);
      nla_put_u8(msg, NL80211_MESH_SETUP_AUTH_PROTOCOL, 0x1);

      // Robust security network information elements (see 802.11-2016
      // section 9.4.2.25 RSNE and Figure 9-255 RSNE format)
      constexpr std::array<unsigned char, 22> RSN_INFORMATION_ELEMENTS{
          0x30, // RSN element ID
          0x14, // Length
          0x1,  0x0, // Version
          0x0,  0x0F, 0xAC, 0x4, // CCMP for group cipher suite
          0x1,  0x0, // Pairwise cipher suite count
          0x0,  0x0F, 0xAC, 0x4, // CCMP for pairwise cipher suite
          0x1,  0x0, // Authentication suite count
          0x0,  0x0F, 0xAC, 0x8, // Using SAE for authentication
          0x0,  0x0, // Capabilities
      };

      nla_put(
          msg,
          NL80211_MESH_SETUP_IE,
          RSN_INFORMATION_ELEMENTS.size(),
          RSN_INFORMATION_ELEMENTS.data());
    }

    nla_nest_end(msg, container);

    int32_t multicastRate = netif.getMeshConfig()->conf->mcast_rate;
    if (multicastRate != 0) {
      nla_put_u32(msg, NL80211_ATTR_MCAST_RATE, multicastRate);
    }
  }
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

status_t
Nl80211Handler::leaveMeshes() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  for (auto& netIntf : netInterfaces_) {
    if (netIntf.second.isValid()) {
      int phyIndex = netIntf.second.phyIndex();
      VLOG(8) << "Attempting to leave mesh " << netIntf.second.meshId << " on "
              << netIntf.second.maybeIfName.value();

      auto linkDown = netIntf.second.bringLinkDown();
      if (linkDown != R_SUCCESS) {
        LOG(ERROR) << "Error bringing down link for phy" << phyIndex;
        return linkDown;
      }

      try {
        leaveMesh(phyIndex);
      } catch (std::runtime_error& e) {
        LOG(ERROR) << "Error leaving mesh on phy" << phyIndex << ": "
                   << e.what();
        return ERR_NETLINK_OTHER;
      }
    }
  }

  return R_SUCCESS;
}

// This method registers with the kernel to send authentication frames using the
// SAE algorithm (used for authenticating mesh peers) to userspace via
// NL80211_CMD_FRAME netlink events. These frames are received via the event
// socket.
void
Nl80211Handler::registerForAuthFrames(const NetInterface& netif) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {})", __func__, netif.phyIndex());

  CHECK(netif.maybeIfIndex.hasValue());

  const uint16_t IEEE80211_FTYPE_MGMT = 0x0000;
  const uint16_t IEEE80211_STYPE_AUTH = 0x00B0;
  uint16_t frameType = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_AUTH;

  // SAE (see 802.11-2016 section 9.4.1.1 Authentication Algorithm Number field)
  char algorithm{0x3};

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_REGISTER_FRAME};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();

  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  nla_put_u16(msg, NL80211_ATTR_FRAME_TYPE, frameType);
  nla_put(msg, NL80211_ATTR_FRAME_MATCH, sizeof(algorithm), &algorithm);
  nlEventSocket_.send(msg);
}

// This method registers with the kernel to send mesh peering action frames
// (mesh peering open/commit/confirm) used for authenticating mesh peers to
// userspace via NL80211_CMD_FRAME netlink events. These frames are received
// via the event socket.
void
Nl80211Handler::registerForMeshPeeringFrames(const NetInterface& netif) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {})", __func__, netif.phyIndex());

  CHECK(netif.maybeIfIndex.hasValue());

  const uint16_t IEEE80211_FTYPE_MGMT = 0x0000;
  const uint16_t IEEE80211_STYPE_ACTION = 0x00D0;
  uint16_t frameType = IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION;

  // Mesh peering open / mesh peering commit / mesh peering confirm
  // (see 802.11-2016 section 9.6.16.1 Self-protected Action fields)
  constexpr std::array<std::array<char, 2>, 3> actionCodes{
      {{15, 1}, {15, 2}, {15, 3}}};

  for (size_t i = 0; i < actionCodes.size(); i++) {
    GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                              NL80211_CMD_REGISTER_FRAME};
    uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
    nla_put_u16(msg, NL80211_ATTR_FRAME_TYPE, frameType);
    nla_put(
        msg,
        NL80211_ATTR_FRAME_MATCH,
        actionCodes[i].size(),
        actionCodes[i].data());
    nlEventSocket_.send(msg);
  }
}

// This method is used to install an encryption key into the driver/firmware.
// Installed keys are then be used for encrypting/decrypting traffic to a
// specific peer (pairwise key) or broadcast (group key).
void
Nl80211Handler::installKey(
    const NetInterface& netif,
    folly::Optional<folly::MacAddress> maybePeer,
    unsigned int cipher,
    unsigned int keyType,
    unsigned char keyIdx,
    unsigned char* keyData) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}"
      "(phy: {}, peer: {}, cipher: {}, keyType: {}, keyIdx: {})",
      __func__,
      netif.phyIndex(),
      maybePeer ? maybePeer->toString() : "none",
      cipher,
      keyType,
      keyIdx);

  CHECK(netif.maybeIfIndex.hasValue());

  // Trying to install a pairwise key without a peer doesn't make sense
  if (keyType == NL80211_KEYTYPE_PAIRWISE) {
    assert(maybePeer);
  }

  std::array<unsigned char, 6> seq{};

  // Create the new key
  {
    GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                              NL80211_CMD_NEW_KEY};
    uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();

    nlattr* container = nla_nest_start(msg, NL80211_ATTR_KEY);
    CHECK_NOTNULL(container);

    int keyLength =
        (cipher == Constants::CIPHER_CCMP ? KEY_LEN_AES_CCMP
                                          : KEY_LEN_AES_CMAC);

    nla_put_u32(msg, NL80211_KEY_CIPHER, cipher);
    nla_put(msg, NL80211_KEY_DATA, keyLength, keyData);
    nla_put_u8(msg, NL80211_KEY_IDX, keyIdx);
    nla_put(msg, NL80211_KEY_SEQ, 6, seq.data());
    nla_put_u32(msg, NL80211_KEY_TYPE, keyType);

    nla_nest_end(msg, container); // NL80211_ATTR_KEY

    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
    if (maybePeer) {
      nla_put(msg, NL80211_ATTR_MAC, ETH_ALEN, maybePeer->bytes());
    }

    VLOG(8) << folly::sformat(
        "Nl80211Handler::{}(): creating key id {}", __func__, keyIdx);
    GenericNetlinkSocket{}.sendAndReceive(msg);
  }

  // Set that as the key to use
  {
    GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                              NL80211_CMD_SET_KEY};
    uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();

    if (cipher == Constants::CIPHER_AES_CMAC) {
      nla_put_flag(msg, NL80211_ATTR_KEY_DEFAULT_MGMT);
    } else {
      nla_put_flag(msg, NL80211_ATTR_KEY_DEFAULT);
    }

    nla_put_u8(msg, NL80211_ATTR_KEY_IDX, keyIdx);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);

    VLOG(8) << folly::sformat(
        "Nl80211Handler::{}(): setting key id {}", __func__, keyIdx);
    GenericNetlinkSocket{}.sendAndReceive(msg);
  }
}

status_t
Nl80211Handler::txFrame(const NetInterface& netif, folly::ByteRange frame) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {}, length: {})",
      __func__,
      netif.phyIndex(),
      frame.size());

  if (!netif.maybeIfIndex) {
    LOG(ERROR) << "Need the interface ifIndex to join mesh";
    return ERR_IFINDEX_NOT_FOUND;
  }
  if (frame.size() == 0) {
    LOG(ERROR) << "Frame length must not be 0";
    return ERR_INVALID_ARGUMENT_VALUE;
  }

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(), NL80211_CMD_FRAME};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  nla_put_u32(msg, NL80211_ATTR_WIPHY_FREQ, netif.frequency);
  nla_put(msg, NL80211_ATTR_FRAME, frame.size(), frame.data());

  VLOG(8) << folly::sformat(
      "txFrame(length: {}): "
      "dest: {} "
      "ifIndex: {} "
      "wiphy_freq: {} ",
      frame.size(),
      folly::MacAddress::fromBinary(
          {reinterpret_cast<const ieee80211_mgmt_frame*>(frame.data())->da,
           ETH_ALEN})
          .toString(),
      ifIndex,
      netif.frequency);
  GenericNetlinkSocket{}.sendAndReceive(msg);
  return R_SUCCESS;
}

// Find out the id of all the multicast groups supported by the nl80211 family
void
Nl80211Handler::getNl80211MulticastGroups() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  GenericNetlinkMessage msg{GenericNetlinkFamily::NLCTRL(), CTRL_CMD_GETFAMILY};
  nla_put_string(msg, CTRL_ATTR_FAMILY_NAME, "nl80211");
  GenericNetlinkSocket{}.sendAndReceive(
      msg, [this](const GenericNetlinkMessage& msg) {
        const auto tb = msg.getAttributes<CTRL_ATTR_MAX>();

        if (!tb[CTRL_ATTR_MCAST_GROUPS]) {
          return NL_SKIP;
        }

        for (const auto& mcgrp :
             NestedNetlinkAttribute{tb[CTRL_ATTR_MCAST_GROUPS]}) {
          TabularNetlinkAttribute<CTRL_ATTR_MCAST_GRP_MAX> tb2{mcgrp};
          if (tb2[CTRL_ATTR_MCAST_GRP_NAME] && tb2[CTRL_ATTR_MCAST_GRP_ID]) {
            auto groupId = uint32_t{nla_get_u32(tb2[CTRL_ATTR_MCAST_GRP_ID])};
            auto groupName =
                std::string{nla_get_string(tb2[CTRL_ATTR_MCAST_GRP_NAME])};
            multicastGroups_[groupName] = groupId;
          }
        }
        for (const auto& v : multicastGroups_) {
          VLOG(8) << v.first;
        }
        return NL_OK;
      });
}

status_t
Nl80211Handler::joinMeshes() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  for (auto& netIntf : netInterfaces_) {
    if (netIntf.second.isValid()) {
      int phyIndex = netIntf.second.phyIndex();
      VLOG(8) << "Attempting to join mesh " << netIntf.second.meshId << " on "
              << netIntf.second.maybeIfName.value();

      auto linkUp = netIntf.second.bringLinkUp();
      if (linkUp != R_SUCCESS) {
        LOG(ERROR) << "Error bringing up link for phy" << phyIndex;
        return linkUp;
      }

      try {
        auto retJoinMesh = initMesh(phyIndex);
        if (retJoinMesh != R_SUCCESS) {
          LOG(ERROR) << "Error initializing mesh on phy" << phyIndex;
          return retJoinMesh;
        }

        joinMesh(phyIndex);
      } catch (std::runtime_error& e) {
        LOG(ERROR) << "Error joining mesh on phy" << phyIndex << ": "
                   << e.what();
        return ERR_NETLINK_OTHER;
      }
    }
  }

  return R_SUCCESS;
}

void
Nl80211Handler::setPlinkState(
    const NetInterface& netif, folly::MacAddress peer, int state) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {}, peer: {}, state: {})",
      __func__,
      netif.phyIndex(),
      peer.toString(),
      state);

  CHECK(netif.maybeIfIndex.hasValue());
  CHECK_GE(state, 0);
  CHECK_LT(state, NUM_NL80211_PLINK_STATES);

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_SET_STATION};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  nla_put(msg, NL80211_ATTR_MAC, ETH_ALEN, peer.bytes());
  nla_put_u8(msg, NL80211_ATTR_STA_PLINK_STATE, state);
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

void
Nl80211Handler::deleteStation(folly::MacAddress peer) {
  const NetInterface& netif = lookupMeshNetif();

  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {}, peer: {})",
      __func__,
      netif.phyIndex(),
      peer.toString());

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_DEL_STATION};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  nla_put(msg, NL80211_ATTR_MAC, ETH_ALEN, peer.bytes());
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

void
Nl80211Handler::setStationAuthenticated(
    const NetInterface& netif, folly::MacAddress peer) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {}, peer: {})",
      __func__,
      netif.phyIndex(),
      peer.toString());

  CHECK(netif.maybeIfIndex.hasValue());

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_SET_STATION};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  nla_put(msg, NL80211_ATTR_MAC, ETH_ALEN, peer.bytes());

  nl80211_sta_flag_update flags;
  flags.mask = flags.set = (1 << NL80211_STA_FLAG_AUTHENTICATED) |
      (1 << NL80211_STA_FLAG_MFP) | (1 << NL80211_STA_FLAG_AUTHORIZED);

  meshd_config* meshd_conf = netif.getMeshConfig()->conf;
  if (!meshd_conf->is_secure || !meshd_conf->pmf) {
    flags.set &= ~(1 << NL80211_STA_FLAG_MFP);
  }

  nla_put(msg, NL80211_ATTR_STA_FLAGS2, sizeof(flags), &flags);
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

status_t
Nl80211Handler::setMeshConfig(
    const NetInterface& netif,
    const authsae_mesh_node& mesh,
    unsigned int changed) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {})", __func__, netif.phyIndex());

  CHECK(netif.maybeIfIndex.hasValue());

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_SET_MESH_CONFIG};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);

  nlattr* container = nla_nest_start(msg, NL80211_ATTR_MESH_CONFIG);
  CHECK_NOTNULL(container);

  if (changed & MESH_CONF_CHANGED_HT) {
    nla_put_u16(msg, NL80211_MESHCONF_HT_OPMODE, mesh.conf->ht_prot_mode);
  }
  nla_nest_end(msg, container);
  GenericNetlinkSocket{}.sendAndReceive(msg);
  return R_SUCCESS;
}

int
Nl80211Handler::getNumberOfMeshPhys() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  int count = 0;
  for (const auto& intf : netInterfaces_) {
    if (intf.second.isMeshCapable) {
      count++;
    }
  }
  return count;
}

void
Nl80211Handler::applyConfiguration() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  auto netIntf = std::begin(netInterfaces_);
  for (; netIntf != std::end(netInterfaces_); ++netIntf) {
    if (!netIntf->second.isMeshCapable ||
        !netIntf->second.isFrequencySupported(FLAGS_mesh_frequency) ||
        netIntf->second.maybeIfName) {
      // This interface doesn't support meshing, doesn't support the requested
      // frequency, or is already in use. Skip.
      continue;
    }

    netIntf->second.meshId = FLAGS_mesh_id;
    netIntf->second.maybeIfName = interfaceName_;

    netIntf->second.frequency = FLAGS_mesh_frequency;
    netIntf->second.centerFreq1 = FLAGS_mesh_center_freq1;
    netIntf->second.centerFreq2 = FLAGS_mesh_center_freq2;
    if (FLAGS_mesh_channel_type.compare("160") == 0) {
      netIntf->second.channelWidth = NL80211_CHAN_WIDTH_160;
    } else if (FLAGS_mesh_channel_type.compare("80+80") == 0) {
      netIntf->second.channelWidth = NL80211_CHAN_WIDTH_80P80;
    } else if (FLAGS_mesh_channel_type.compare("80") == 0) {
      netIntf->second.channelWidth = NL80211_CHAN_WIDTH_80;
    } else if (FLAGS_mesh_channel_type.compare("40") == 0) {
      netIntf->second.channelWidth = NL80211_CHAN_WIDTH_40;
    } else if (FLAGS_mesh_channel_type.compare("20") == 0) {
      netIntf->second.channelWidth = NL80211_CHAN_WIDTH_20;
    } else if (FLAGS_mesh_channel_type.compare("NOHT") == 0) {
      netIntf->second.channelWidth = NL80211_CHAN_WIDTH_20_NOHT;
    } else {
      throw std::invalid_argument(folly::sformat(
          "Invalid channel type {} in config", FLAGS_mesh_channel_type));
    }

    netIntf->second.isEncrypted = FLAGS_enable_encryption;
    netIntf->second.encryptionPassword = FLAGS_encryption_password;
    netIntf->second.encryptionSaeGroups = parseCsvFlag<int>(
        FLAGS_encryption_sae_groups, [](std::string str) { return stoi(str); });
    netIntf->second.encryptionDebug = FLAGS_userspace_peering_verbosity;

    netIntf->second.maybeRssiThreshold = FLAGS_mesh_rssi_threshold;

    if (!FLAGS_mesh_mac_address.empty()) {
      netIntf->second.maybeMacAddress =
          folly::MacAddress{FLAGS_mesh_mac_address};
    } else {
      VLOG(8) << "macAddress was not provided in mesh config";
    }
    netIntf->second.ttl = static_cast<uint8_t>(FLAGS_mesh_ttl);
    netIntf->second.elementTtl = static_cast<uint8_t>(FLAGS_mesh_ttl_element);
    netIntf->second.hwmpActivePathTimeout =
        static_cast<uint32_t>(FLAGS_mesh_hwmp_active_path_timeout);
    netIntf->second.hwmpRannInterval =
        static_cast<uint32_t>(FLAGS_mesh_hwmp_rann_interval);
    netIntf->second.maxPeerLinks =
        static_cast<uint16_t>(FLAGS_mesh_max_peer_links);
    try {
      // We create this interface ourselves, and if someone else already
      // created one with this name it causes problems. Delete the interface
      // if it already exists.
      deleteInterface(netIntf->second.maybeIfName.value());

      netIntf->second.maybeMacAddress =
          createInterface(netIntf->second.phyIndex());
      VLOG(8) << folly::sformat(
          "Created interface '{}' with MAC '{}'",
          netIntf->second.maybeIfName.value(),
          netIntf->second.maybeMacAddress.value().toString());
    } catch (std::runtime_error& e) {
      LOG(ERROR)
          << folly::sformat(
                 "Error creating interface {}", netIntf->second.phyIndex())
          << " error: " << this << e.what();
    }
    VLOG(8) << "Mapped ifName " << interfaceName_ << " to "
            << netIntf->second.phyName;

    break;
  }
  if (netIntf == std::end(netInterfaces_)) {
    this->tearDown();
    throw std::runtime_error(
        "Failed to apply interface configuration to the detected phys");
  }
}

void
Nl80211Handler::tearDown() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  for (auto& phy : netInterfaces_) {
    for (int idx = 0; idx < IEEE80211_NUM_BANDS; idx++) {
      std::free(phy.second.getMeshConfig()->bands[idx].rates);
      phy.second.getMeshConfig()->bands[idx].rates = nullptr;
    }
    if (phy.second.maybeIfName) {
      try {
        deleteInterface(phy.second.maybeIfName.value());

      } catch (std::runtime_error& e) {
        LOG(ERROR) << folly::sformat(
            "Error tearing down interface '{}': {}",
            phy.second.maybeIfName.value(),
            e.what());
      }
    }
  }
}

Nl80211Handler::~Nl80211Handler() {
  tearDown();
}

status_t
Nl80211Handler::handleNewCandidate(const GenericNetlinkMessage& msg) {
  VLOG(8) << folly::sformat("::{}()", __func__);

  const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

  // Check that all the required info exists:
  //   - source address (arrives as bssid)
  //   - meshid
  //   - mesh config
  //   - RSN
  if (!tb[NL80211_ATTR_MAC] || !tb[NL80211_ATTR_IE]) {
    return ERR_INVALID_ARGUMENT_VALUE;
  }

  uint32_t phyIndex = *(uint32_t*)nla_data(tb[NL80211_ATTR_WIPHY]);
  const NetInterface& netif =
      Nl80211Handler::globalNlHandler->lookupNetifFromPhy(phyIndex);

  unsigned char* ie = (unsigned char*)nla_data(tb[NL80211_ATTR_IE]);
  size_t ie_len = nla_len(tb[NL80211_ATTR_IE]);

  info_elems elems;
  parse_ies(ie, ie_len, &elems);

  auto mac_addr = folly::MacAddress::fromBinary(
      {static_cast<unsigned char*>(nla_data(tb[NL80211_ATTR_MAC])), ETH_ALEN});

  VLOG(8) << folly::sformat(
      "Processing new candidate with MAC {}", mac_addr.toString());

  meshd_config* meshd_conf = netif.getMeshConfig()->conf;

  if (elems.mesh_id == nullptr || elems.mesh_id_len != meshd_conf->meshid_len ||
      memcmp(elems.mesh_id, meshd_conf->meshid, meshd_conf->meshid_len) != 0) {
    std::string candidateMeshId{reinterpret_cast<char*>(elems.mesh_id),
                                elems.mesh_id_len};
    VLOG(8) << folly::sformat(
        "Ignoring candidate: different mesh ID '{}'", candidateMeshId);
    return R_SUCCESS;
  }

  if (elems.rsn == nullptr && meshd_conf->is_secure) {
    VLOG(8) << "Ignoring candidate: no RSN IE provided";
    return ERR_INVALID_ARGUMENT_VALUE;
  }

  if (!FLAGS_mesh_init_peering_whitelist.empty()) {
    auto allowed_peers = ::parseCsvFlag<folly::MacAddress>(
        FLAGS_mesh_init_peering_whitelist,
        [](std::string str) { return folly::MacAddress{str}; });

    if (std::find(allowed_peers.begin(), allowed_peers.end(), mac_addr) ==
        allowed_peers.end()) {
      VLOG(8)
          << "Ignoring candidate: not in list of stations to initate peering with";
      return ERR_INVALID_ARGUMENT_VALUE;
    }
  }

  ieee80211_mgmt_frame bcn;
  memset(&bcn, 0, sizeof(bcn));
  bcn.frame_control =
      htole16(IEEE802_11_FC_TYPE_MGMT << 2 | IEEE802_11_FC_STYPE_BEACON << 4);
  memcpy(bcn.sa, nla_data(tb[NL80211_ATTR_MAC]), ETH_ALEN);

  if (process_mgmt_frame(
          &bcn,
          sizeof(bcn),
          (unsigned char*)netif.maybeMacAddress->bytes(),
          /*cookie*/ nullptr,
          !netif.isEncrypted) != 0) {
    VLOG(8) << "libsae: process_mgmt_frame failed";
    return ERR_AUTHSAE;
  }

  // If peer now exists, we know it was created by process_mgmt_frame, or if
  // we received two NEW_PEER_CANDIDATE events for the same peer, this will
  // fail
  if (candidate* created_peer = find_peer(bcn.sa, 0)) {
    ampe_set_peer_ies(created_peer, &elems);

    if (!created_peer->in_kernel) {
      Nl80211Handler::globalNlHandler->createUnauthenticatedStation(
          netif, folly::MacAddress::fromBinary({bcn.sa, ETH_ALEN}), elems);
      created_peer->in_kernel = true;
    }
  }

  return R_SUCCESS;
}

status_t
Nl80211Handler::handleDeletedPeer(const GenericNetlinkMessage& msg) {
  VLOG(8) << folly::sformat("::{}()", __func__);

  const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

  if (!lookupMeshNetif().maybeIfIndex ||
      nla_get_u32(tb[NL80211_ATTR_IFINDEX]) !=
          static_cast<uint32_t>(lookupMeshNetif().maybeIfIndex.value())) {
    return ERR_IFINDEX_NOT_FOUND;
  }

  if (!tb[NL80211_ATTR_MAC] || nla_len(tb[NL80211_ATTR_MAC]) != ETH_ALEN) {
    return ERR_NETLINK_OTHER;
  }

  if (candidate* peer = find_peer(
          static_cast<unsigned char*>(nla_data(tb[NL80211_ATTR_MAC])), false)) {
    ampe_close_peer_link(peer->peer_mac);
    delete_peer(&peer);
  }

  return R_SUCCESS;
}

int
Nl80211Handler::processEvent(const GenericNetlinkMessage& msg) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  auto cmd = msg.getGenericHeader()->cmd;
  const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

  if (tb[NL80211_ATTR_WIPHY]) {
    uint32_t phyIndex =
        *static_cast<uint32_t*>(nla_data(tb[NL80211_ATTR_WIPHY]));

    VLOG(10)
        << folly::sformat("Received netlink event {} for phy{}", cmd, phyIndex);

    if (netInterfaces_.count(phyIndex) == 0) {
      VLOG(10) << folly::sformat(
          "No phy{} interface exists, ignoring event", phyIndex);
      return NL_SKIP;
    }
  } else {
    VLOG(10) << folly::sformat(
        "Received netlink event {} not associated with a phy", cmd);
  }

  switch (cmd) {
  case NL80211_CMD_NEW_PEER_CANDIDATE:
    VLOG(5) << "Processing NL80211_CMD_NEW_PEER_CANDIDATE event";
    handleNewCandidate(msg);
    break;

  case NL80211_CMD_FRAME:
    if (tb[NL80211_ATTR_FRAME] && nla_len(tb[NL80211_ATTR_FRAME])) {
      VLOG(5) << "Processing NL80211_CMD_FRAME event";

      ieee80211_mgmt_frame* frame =
          static_cast<ieee80211_mgmt_frame*>(nla_data(tb[NL80211_ATTR_FRAME]));
      int frame_len = nla_len(tb[NL80211_ATTR_FRAME]);

      // Auth frames should be handled by authsae
      unsigned short frame_control = ieee_order(frame->frame_control);
      uint16_t type = IEEE802_11_FC_GET_TYPE(frame_control);
      uint16_t subtype = IEEE802_11_FC_GET_STYPE(frame_control);

      if (type == IEEE802_11_FC_TYPE_MGMT &&
          (subtype == IEEE802_11_FC_STYPE_ACTION ||
           subtype == IEEE802_11_FC_STYPE_AUTH)) {
        int32_t rssi{};

        if (tb[NL80211_ATTR_RX_SIGNAL_DBM]) {
          rssi = nla_get_u32(tb[NL80211_ATTR_RX_SIGNAL_DBM]);
        }
        // drop frames below rssi threshold, except for close frames
        // because we don't want to keep estab stations on bad links
        if (rssi < FLAGS_mesh_rssi_threshold &&
            !(frame_len >= (int)sizeof(struct ieee80211_mgmt_frame) &&
              frame->action.action_code == PLINK_CLOSE)) {
          VLOG(8) << folly::sformat(
              "Ignoring non-close frame below rssi threshold ({} < {})",
              rssi,
              FLAGS_mesh_rssi_threshold);
          break;
        }
        const NetInterface& netif = lookupMeshNetif();
        if (process_mgmt_frame(
                frame,
                frame_len,
                frame->da,
                /*cookie*/ nullptr,
                !netif.getMeshConfig()->conf->is_secure)) {
          LOG(ERROR) << "process_mgmt_frame failed";
        }
      } else
        VLOG(8) << "Got unexpected frame, ignoring";
    }
    break;

  case NL80211_CMD_FRAME_TX_STATUS:
    VLOG(5) << "Processing NL80211_CMD_FRAME_TX_STATUS event";
    if (!tb[NL80211_ATTR_ACK]) {
      LOG(ERROR) << "Frame transmission failed: did not receive ACK";
    }
    if (!tb[NL80211_ATTR_FRAME]) {
      LOG(ERROR) << "Frame transmission failed: no frame contents";
    }
    break;

  case NL80211_CMD_DEL_STATION:
    VLOG(5) << "Processing NL80211_CMD_DEL_STATION event";
    handleDeletedPeer(msg);
    break;

  default:
    VLOG(8) << folly::sformat("Ignored unhandled event ({})", cmd);
    break;
  }

  return NL_SKIP;
}

void
Nl80211Handler::createUnauthenticatedStation(
    const NetInterface& netif,
    folly::MacAddress peer,
    const info_elems& elems) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(phy: {}, peer: {})",
      __func__,
      netif.phyIndex(),
      peer.toString());

  CHECK(netif.maybeIfIndex.hasValue());

  uint16_t peerAid =
      find_peer(
          const_cast<unsigned char*>(peer.bytes()), /* onlyAccepted */ false)
          ->association_id;
  VLOG(8) << folly::sformat("Peer's AID is {}", peerAid);

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_NEW_STATION};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  nla_put(msg, NL80211_ATTR_MAC, ETH_ALEN, peer.bytes());

  if (elems.sup_rates) {
    nla_put(
        msg,
        NL80211_ATTR_STA_SUPPORTED_RATES,
        elems.sup_rates_len,
        elems.sup_rates);
  }

  nl80211_sta_flag_update flags;
  flags.mask =
      (1 << NL80211_STA_FLAG_AUTHENTICATED) | (1 << NL80211_STA_FLAG_WME);
  flags.set = (1 << NL80211_STA_FLAG_WME);
  nla_put(msg, NL80211_ATTR_STA_FLAGS2, sizeof(flags), &flags);

  nla_put_u16(msg, NL80211_ATTR_STA_AID, peerAid);
  nla_put_u16(msg, NL80211_ATTR_STA_LISTEN_INTERVAL, 100);

  // Unset 20/40 MHz capability in ht_cap if HT operation IE indicates this is
  // a 20 MHz STA
  if (elems.ht_info &&
      !(((ht_op_ie*)elems.ht_info)->ht_param &
        IEEE80211_HT_PARAM_CHAN_WIDTH_ANY)) {
    ((ht_cap_ie*)elems.ht_cap)->cap_info &= ~IEEE80211_HT_CAP_SUP_WIDTH_20_40;
  }

  if (elems.ht_cap) {
    nla_put(msg, NL80211_ATTR_HT_CAPABILITY, elems.ht_cap_len, elems.ht_cap);
  }
  if (elems.vht_cap) {
    nla_put(msg, NL80211_ATTR_VHT_CAPABILITY, elems.vht_cap_len, elems.vht_cap);
  }
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

std::ostream&
openr::fbmeshd::operator<<(std::ostream& os, const Nl80211Handler& nl) {
  for (const auto& mc : nl.netInterfaces_) {
    os << "key: " << std::to_string(mc.first) << std::endl;
    os << mc.second << std::endl;
    os << "-------------" << std::endl;
  }
  return os;
}

thrift::Mesh
Nl80211Handler::getMesh() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}", __func__);

  const NetInterface& netif = lookupMeshNetif();

  thrift::Mesh mesh;

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_GET_INTERFACE,
                            NLM_F_DUMP | NLM_F_ACK};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  GenericNetlinkSocket{}.sendAndReceive(
      msg, [&mesh](const GenericNetlinkMessage& msg) {
        const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

        if (!tb[NL80211_ATTR_WIPHY_FREQ]) {
          LOG(INFO) << "freq info missing";
          return NL_SKIP;
        }

        mesh.frequency = nla_get_u32(tb[NL80211_ATTR_WIPHY_FREQ]);

        if (!tb[NL80211_ATTR_WIPHY_TX_POWER_LEVEL]) {
          LOG(INFO) << "tx power info missing";
          return NL_SKIP;
        }

        mesh.txPower = nla_get_u32(tb[NL80211_ATTR_WIPHY_TX_POWER_LEVEL]);

        if (!tb[NL80211_ATTR_CENTER_FREQ1]) {
          LOG(INFO) << "center_freq_1 info missing";
          return NL_SKIP;
        } else {
          mesh.centerFreq1 = nla_get_u32(tb[NL80211_ATTR_CENTER_FREQ1]);
        }

        if (!tb[NL80211_ATTR_CENTER_FREQ2]) {
          LOG(INFO) << "center_freq_2 info missing";
        } else {
          mesh.centerFreq2 = nla_get_u32(tb[NL80211_ATTR_CENTER_FREQ2]);
        }

        if (!tb[NL80211_ATTR_CHANNEL_WIDTH]) {
          LOG(INFO) << "channel_width info missing";
          return NL_SKIP;
        } else {
          mesh.channelWidth = nla_get_u32(tb[NL80211_ATTR_CHANNEL_WIDTH]);
        }

        return NL_OK;
      });

  return mesh;
}

std::vector<StationInfo>
Nl80211Handler::getStationsInfo() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}()", __func__);

  const NetInterface& netif = lookupMeshNetif();

  std::vector<StationInfo> stationsInfo;

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_GET_STATION,
                            NLM_F_DUMP | NLM_F_ACK};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  GenericNetlinkSocket{}.sendAndReceive(
      msg, [&stationsInfo](const GenericNetlinkMessage& msg) {
        const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

        if (!tb[NL80211_ATTR_STA_INFO]) {
          LOG(INFO) << "Station stats missing, skipping";
          return NL_SKIP;
        }

        TabularNetlinkAttribute<NL80211_STA_INFO_MAX> sinfo{
            tb[NL80211_ATTR_STA_INFO], stats_policy_};

        if (sinfo[NL80211_STA_INFO_PLINK_STATE]) {
          if (nla_get_u8(sinfo[NL80211_STA_INFO_PLINK_STATE]) == PLINK_ESTAB) {
            auto mac_addr = folly::MacAddress::fromBinary(
                {static_cast<unsigned char*>(nla_data(tb[NL80211_ATTR_MAC])),
                 ETH_ALEN});
            uint32_t inactive_time =
                nla_get_u32(sinfo[NL80211_STA_INFO_INACTIVE_TIME]);

            int8_t rssi{0};
            if (sinfo[NL80211_STA_INFO_SIGNAL_AVG]) {
              rssi = static_cast<int8_t>(
                  nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL_AVG]));
              if (rssi == 0) {
                LOG(INFO) << "Station RSSI invalid, skipping";
                return NL_SKIP;
              }
            } else {
              LOG(INFO) << "Station RSSI missing, skipping";
              return NL_SKIP;
            }

            uint32_t expectedThroughput{0};
            if (sinfo[NL80211_STA_INFO_EXPECTED_THROUGHPUT]) {
              expectedThroughput =
                  nla_get_u32(sinfo[NL80211_STA_INFO_EXPECTED_THROUGHPUT]);
            }

            bool isConnectedToGate = false;
            if (sinfo[NL80211_STA_INFO_CONNECTED_TO_GATE]) {
              isConnectedToGate =
                  nla_get_u8(sinfo[NL80211_STA_INFO_CONNECTED_TO_GATE]);
            }

            stationsInfo.push_back(
                StationInfo{mac_addr,
                            std::chrono::milliseconds{inactive_time},
                            rssi,
                            isConnectedToGate,
                            expectedThroughput});
          }
        }

        return NL_OK;
      });

  return stationsInfo;
}

std::vector<folly::MacAddress>
Nl80211Handler::getPeers() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}", __func__);

  std::vector<folly::MacAddress> peers;
  for (const auto& it : getStationsInfo()) {
    peers.push_back(it.macAddress);
  }
  return peers;
}

std::unordered_map<folly::MacAddress, int32_t>
Nl80211Handler::getMetrics() {
  VLOG(8) << folly::sformat("Nl80211Handler::{}", __func__);

  const NetInterface& netif = lookupMeshNetif();

  std::unordered_map<folly::MacAddress, int32_t> metrics;

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_GET_MPATH,
                            NLM_F_DUMP | NLM_F_ACK};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);
  GenericNetlinkSocket{}.sendAndReceive(
      msg, [&metrics](const GenericNetlinkMessage& msg) {
        const auto tb = msg.getAttributes<NL80211_ATTR_MAX>();

        if (!tb[NL80211_ATTR_MPATH_INFO]) {
          LOG(INFO) << "mpath info missing";
          return NL_SKIP;
        }

        TabularNetlinkAttribute<NL80211_MPATH_INFO_MAX> pinfo{
            tb[NL80211_ATTR_MPATH_INFO], mpath_policy_};

        const auto mac_addr = folly::MacAddress::fromBinary(
            {static_cast<unsigned char*>(nla_data(tb[NL80211_ATTR_MAC])),
             ETH_ALEN});

        // NL80211_MPATH_INFO_EXPTIME=0 means an expired path. So don't report.
        if (nla_get_u32(pinfo[NL80211_MPATH_INFO_EXPTIME]) > 0) {
          metrics.emplace(
              mac_addr, nla_get_u32(pinfo[NL80211_MPATH_INFO_METRIC]));
        }

        return NL_SKIP;
      });

  return metrics;
}

void
Nl80211Handler::setMeshConnectedToGate(bool isConnected) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(isConnected: {})", __func__, isConnected);

  const NetInterface& netif = lookupMeshNetif();

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_SET_MESH_CONFIG};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);

  nlattr* container = nla_nest_start(msg, NL80211_ATTR_MESH_CONFIG);
  CHECK_NOTNULL(container);
  nla_put_u8(msg, NL80211_MESHCONF_CONNECTED_TO_GATE, isConnected);

  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(name: {}): "
      "ifindex: {} "
      "connectedToGate: {} ",
      __func__,
      netif.maybeIfName.value(),
      ifIndex,
      isConnected);
  nla_nest_end(msg, container);
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

void
Nl80211Handler::setRootMode(uint8_t mode) {
  VLOG(8) << folly::sformat("Nl80211Handler::{}(mode: {})", __func__, mode);

  const NetInterface& netif = lookupMeshNetif();

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_SET_MESH_PARAMS};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);

  nlattr* container = nla_nest_start(msg, NL80211_ATTR_MESH_PARAMS);
  CHECK_NOTNULL(container);
  nla_put_u8(msg, NL80211_MESHCONF_HWMP_ROOTMODE, mode);

  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(name: {}): "
      "ifindex: {} "
      "mode: {} ",
      __func__,
      netif.maybeIfName.value(),
      ifIndex,
      mode);
  nla_nest_end(msg, container);
  GenericNetlinkSocket{}.sendAndReceive(msg);
}

void
Nl80211Handler::setRssiThreshold(int32_t rssiThreshold) {
  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(rssiThreshold: {})", __func__, rssiThreshold);

  const NetInterface& netif = lookupMeshNetif();

  GenericNetlinkMessage msg{GenericNetlinkFamily::NL80211(),
                            NL80211_CMD_SET_MESH_CONFIG};
  uint32_t ifIndex = (uint32_t)netif.maybeIfIndex.value();
  nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifIndex);

  nlattr* container = nla_nest_start(msg, NL80211_ATTR_MESH_CONFIG);
  CHECK_NOTNULL(container);
  nla_put_u32(msg, NL80211_MESHCONF_RSSI_THRESHOLD, rssiThreshold);

  VLOG(8) << folly::sformat(
      "Nl80211Handler::{}(name: {}): "
      "ifindex: {} "
      "rssiThreshold: {} ",
      __func__,
      netif.maybeIfName.value(),
      ifIndex,
      rssiThreshold);
  nla_nest_end(msg, container);
  GenericNetlinkSocket{}.sendAndReceive(msg);
}
