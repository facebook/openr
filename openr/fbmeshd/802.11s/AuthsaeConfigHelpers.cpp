/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AuthsaeConfigHelpers.h"

#include <glog/logging.h>

#include <folly/Format.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>

using namespace openr::fbmeshd;

namespace {

static int
set_11a_rates(authsae_meshd_config* mconf, u16* rates, size_t rates_len) {
  const char basic = 0x80;
  int want = 3;
  for (size_t i = 0; i < rates_len; i++) {
    if (rates[i] == 60 || rates[i] == 120 || rates[i] == 240) {
      mconf->rates[i] |= basic;
      want--;
    }
  }
  return want;
}

static int
set_11g_rates(authsae_meshd_config* mconf, u16* rates, size_t rates_len) {
  const char basic = 0x80;
  int want = 7;
  for (size_t i = 0; i < rates_len; i++) {
    if (rates[i] == 10 || rates[i] == 20 || rates[i] == 55 || rates[i] == 60 ||
        rates[i] == 110 || rates[i] == 120 || rates[i] == 240) {
      mconf->rates[i] |= basic;
      want--;
    }
  }
  return want;
}

} // namespace

// This function is copied from meshd-nl80211.c in authsae, where it is called
// set_sup_basic_rates().
//
// Notes from the original: copy the phy supported rate set into the mesh conf,
// and hardcode BSSBasicRateSet as the mandatory phy rates for now
// TODO: allow user to configure BSSBasicRateSet
//
// TODO 2018-05-23: This would be good to move into authsae as it depends on
// authsae's internal config data format.
void
internal::setSupportedBasicRates(
    int phyIndex, authsae_meshd_config* mconf, u16* rates, size_t rates_len) {
  VLOG(1) << folly::sformat("::{}(rates_len: {})", __func__, rates_len);

  memset(mconf->rates, 0, sizeof(mconf->rates));
  assert(sizeof(mconf->rates) >= rates_len);

  for (size_t i = 0; i < rates_len; i++) {
    // nl80211 reports in 100kb/s, IEEE 802.11 is 500kb/s
    mconf->rates[i] = (uint8_t)(rates[i] / 5);
  }

  int want;

  switch (mconf->band) {
  case MESHD_11a:
    want = set_11a_rates(mconf, rates, rates_len);
    if (want != 0) {
      throw std::runtime_error(folly::sformat(
          "phy{} does not support the expected basic rate set for the 5GHz "
          "band",
          phyIndex));
    }
    break;
  case MESHD_11b:
  case MESHD_11g:
    want = set_11g_rates(mconf, rates, rates_len);
    if (want != 0 && want != 3 && want != 6) {
      throw std::runtime_error(folly::sformat(
          "phy{} does not support the expected basic rate set for the 2.4GHz "
          "band",
          phyIndex));
    }
    break;
  }
}

// Generates an sae_config struct using the mesh configuration from an encrypted
// NetInterface. Note that the struct is entirely generated in this function and
// returned by value.
authsae_sae_config
createSaeConfig(const NetInterface& netif) {
  VLOG(1) << folly::sformat(
      "AuthsaeConfigHelpers::{}(phy: {})", __func__, netif.phyIndex());

  authsae_sae_config saeConfig;
  memset(&saeConfig, 0, sizeof(saeConfig));

  for (size_t i = 0; i < netif.encryptionSaeGroups.size(); i++) {
    saeConfig.group[i] = netif.encryptionSaeGroups[i];
  }
  saeConfig.num_groups = netif.encryptionSaeGroups.size();

  memcpy(
      saeConfig.pwd,
      netif.encryptionPassword.data(),
      netif.encryptionPassword.length());

  saeConfig.debug = netif.encryptionDebug;
  saeConfig.retrans = 0;
  // TODO 2018-06-20: Effectively disable rekeying pending further testing
  saeConfig.pmk_expiry = INT_MAX;
  saeConfig.open_threshold = 5;
  saeConfig.blacklist_timeout = 5;
  saeConfig.giveup_threshold = 0;

  return saeConfig;
}

// Generates a mesh_node struct (and its corresponding meshd_config struct)
// using the mesh configuration from an encrypted NetInterface. Note that the
// structs are pre-constructed as part of the NetInterface.
authsae_mesh_node*
createMeshConfig(NetInterface& netif) {
  VLOG(1) << folly::sformat(
      "AuthsaeConfigHelpers::{}(phy: {})", __func__, netif.phyIndex());

  authsae_mesh_node* mesh = netif.getMeshConfig();

  const int IEEE80211_BAND_2GHZ_MAX_FREQUENCY = 2484;
  if (netif.frequency > IEEE80211_BAND_2GHZ_MAX_FREQUENCY) {
    mesh->band = IEEE80211_BAND_5GHZ;
    mesh->conf->band = MESHD_11a;
  } else {
    mesh->band = IEEE80211_BAND_2GHZ;
    mesh->conf->band = MESHD_11g;
  }

  memcpy(mesh->conf->meshid, netif.meshId.data(), netif.meshId.length());
  mesh->conf->meshid_len = netif.meshId.length();

  memcpy(
      mesh->conf->interface,
      netif.maybeIfName.value().data(),
      netif.maybeIfName.value().length());

  mesh->conf->is_secure = netif.isEncrypted;
  mesh->conf->passive = 0;
  mesh->conf->mediaopt = 0;
  mesh->conf->control_freq = netif.frequency;
  mesh->conf->center_freq1 = netif.centerFreq1;
  mesh->conf->center_freq2 = netif.centerFreq2;
  mesh->conf->debug = 0;
  mesh->conf->channel_width =
      static_cast<enum channel_width>(netif.channelWidth);
  mesh->conf->ht_prot_mode = 3;
  mesh->conf->pmf = 1;
  mesh->conf->mcast_rate = 120; // in 100s of kbps (120 => 12 Mbps)
  mesh->conf->max_plinks = netif.maxPeerLinks;
  mesh->conf->beacon_interval = -1;
  mesh->conf->path_refresh_time = -1;
  mesh->conf->min_discovery_timeout = -1;
  mesh->conf->gate_announcements = -1;
  mesh->conf->hwmp_active_path_timeout = -1;
  mesh->conf->hwmp_net_diameter_traversal_time = -1;
  mesh->conf->hwmp_rootmode = -1;
  mesh->conf->hwmp_rann_interval = -1;
  mesh->conf->hwmp_active_path_to_root_timeout = -1;
  mesh->conf->hwmp_root_interval = -1;
  mesh->conf->rekey_enable = 0;
  mesh->conf->rekey_multicast_group_family = 2;
  memcpy(&mesh->conf->rekey_multicast_group_address, "\xe0\x00\x00\xc8", 4);
  mesh->conf->rekey_ping_port = 0xb13; // in network byte order
  mesh->conf->rekey_pong_port = 0xc13; // in network byte order
  mesh->conf->rekey_ping_count_max = 32;
  mesh->conf->rekey_ping_timeout = 500; // in msec
  mesh->conf->rekey_ping_jitter = 100; // in msec
  mesh->conf->rekey_reauth_count_max = 8;
  mesh->conf->rekey_ok_ping_count_max = 16;

  // If this hits, no rates were reported when the NetInterface was created
  assert(netif.getSupportedBand(mesh->band)->rates);

  // Configure BSSBasicRateSet
  internal::setSupportedBasicRates(
      netif.phyIndex(),
      mesh->conf,
      netif.getSupportedBand(mesh->band)->rates,
      netif.getSupportedBand(mesh->band)->n_bitrates);

  return mesh;
}
