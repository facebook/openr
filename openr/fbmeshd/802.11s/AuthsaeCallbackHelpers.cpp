/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AuthsaeCallbackHelpers.h"

extern "C" {
#include <authsae/ampe.h>
#include <authsae/evl_ops.h>
#include <authsae/sae.h>
}

#include <glog/logging.h>
#include <openssl/rand.h>
#include <algorithm>

#include <folly/Format.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/common/Constants.h>

using namespace openr::fbmeshd;

// Event loop pointer to allow control over additional sockets, timeouts, etc.
fbzmq::ZmqEventLoop* AuthsaeCallbackHelpers::zmqLoop_{nullptr};

// These static C functions are declared and expected by authsae for the
// encrypted mesh functionality. They serve as forwarders to the current
// (single) instance of Nl80211Handler.

// Used for SAE
static int
meshd_write_mgmt(char* frame, int framelen, void* /* cookie */) {
  VLOG(1) << folly::sformat("authsae: {}(framelen: {})", __func__, framelen);

  if (!frame) {
    LOG(WARNING) << folly::sformat(
        "authsae: {}() got a null frame pointer, not transmitting anything",
        __func__);
    return 0;
  }

  if (framelen < 0) {
    LOG(WARNING) << folly::sformat(
        "authsae: {}() got a negative framelen {}, not transmitting anything",
        __func__,
        framelen);
    return 0;
  }

  Nl80211Handler::globalNlHandler->txFrame(
      Nl80211Handler::globalNlHandler->lookupMeshNetif(),
      {reinterpret_cast<unsigned char*>(frame), static_cast<size_t>(framelen)});

  return framelen;
}

static void
peer_created(unsigned char* peer_mac) {
  VLOG(1) << folly::sformat(
      "authsae: {}(peer_mac: {})",
      __func__,
      folly::MacAddress::fromBinary({peer_mac, ETH_ALEN}).toString());

  auto peer = folly::MacAddress::fromBinary({peer_mac, ETH_ALEN});

  //  We don't have to do anything here:
  //
  //  - if peer is created from SAE state machine, then it will be added to the
  //    kernel when the IEs are known (i.e. on estab plink)
  //
  //  - if peer is created from a beacon, we will know the IEs and will go ahead
  //    and create the peer in handleNewCandidate().

  // Validate the invariant that this is only called before the peer is reported
  // to the kernel.
  candidate* cand = find_peer(const_cast<unsigned char*>(peer.bytes()), 0);
  if (cand && cand->in_kernel) {
    LOG(WARNING) << folly::sformat(
        "Attempted to create a peer already in the kernel: {}",
        peer.toString());
  }
}

static void
delete_peer_by_addr(unsigned char* peer_mac) {
  candidate* cand = find_peer(peer_mac, 0);
  if (!cand) {
    return;
  }

  if (cand->in_kernel) {
    /*
     * remove from kernel; will be removed from SAE db after
     * kernel delivers DEL_STATION event.
     */
    Nl80211Handler::globalNlHandler->deleteStation(
        folly::MacAddress::fromBinary({peer_mac, ETH_ALEN}));
  } else {
    /* remove from SAE db only */
    delete_peer(&cand);
  }
}

static void
fin(unsigned short reason,
    unsigned char* peer_mac,
    unsigned char* key,
    int keylen,
    void* /* cookie */) {
  VLOG(1) << folly::sformat(
      "authsae: {}(reason: {}, peer_mac: {})",
      __func__,
      reason,
      folly::MacAddress::fromBinary({peer_mac, ETH_ALEN}).toString());

  auto& netif = Nl80211Handler::globalNlHandler->lookupMeshNetif();

  if (reason == 0) {
    if (keylen > 0) {
      if (netif.getMeshConfig()->conf->is_secure) {
        // Format the key into a human-readable string
        std::string keyString;
        keyString.reserve(keylen * 2);
        for (int i = 0; i < keylen; i++) {
          keyString.append(folly::sformat("{:0x}", key[i]));
        }

        VLOG(1) << folly::sformat("SAE completed with key: {}", keyString);
      }

      ampe_open_peer_link(peer_mac, /*cookie*/ nullptr);
    } else {
      LOG(WARNING) << "SAE completed successfully without returning a key.";
    }
  } else {
    delete_peer_by_addr(peer_mac);
  }
}

// Used for AMPE
static int
meshd_set_mesh_conf(mesh_node* mesh, uint32_t changed) {
  VLOG(1) << folly::sformat("authsae: {}()", __func__);
  CHECK_NOTNULL(mesh);

  if (Nl80211Handler::globalNlHandler->lookupMeshNetif().getMeshConfig() !=
      mesh) {
    LOG(ERROR) << "Authsae config from current mesh netif doesn't match config "
                  "provided by authsae";
  }

  Nl80211Handler::globalNlHandler->setMeshConfig(
      Nl80211Handler::globalNlHandler->lookupMeshNetif(), *mesh, changed);
  return 0;
}

static int
set_plink_state(unsigned char* peer, int state, void* /* cookie */) {
  VLOG(1) << folly::sformat(
      "authsae: {}(peer: {}, state: {})",
      __func__,
      folly::MacAddress::fromBinary({peer, ETH_ALEN}).toString(),
      state);

  Nl80211Handler::globalNlHandler->setPlinkState(
      Nl80211Handler::globalNlHandler->lookupMeshNetif(),
      folly::MacAddress::fromBinary({peer, ETH_ALEN}),
      state);

  return 0;
}

static void
estab_peer_link(
    unsigned char* peer_mac,
    unsigned char* mtk,
    int mtk_len,
    unsigned char* peer_mgtk,
    int peer_mgtk_len,
    unsigned int /* mgtk_expiration */,
    unsigned char* peer_igtk,
    int peer_igtk_len,
    int peer_igtk_keyid,
    unsigned char* /* rates */,
    unsigned short rates_len,
    void* /* cookie */) {
  VLOG(1) << folly::sformat(
      "authsae: {}(peer_mac: {}, igtk_keyid: {}, rates_len: {})",
      __func__,
      folly::MacAddress::fromBinary({peer_mac, ETH_ALEN}).toString(),
      peer_igtk_keyid,
      rates_len);

  if (!peer_mac) {
    return;
  }
  auto peer = folly::MacAddress::fromBinary({peer_mac, ETH_ALEN});

  candidate* cand = find_peer(const_cast<unsigned char*>(peer.bytes()), 0);
  if (!cand) {
    return;
  }

  if (!cand->conf) {
    // try getting 'accepted' peer if we got here without ampe state
    cand = find_peer(const_cast<unsigned char*>(peer.bytes()), 1);
  }

  if (!cand->conf) {
    return;
  }

  meshd_config* meshd_conf = cand->conf->mesh->conf;

  if (meshd_conf->is_secure) {
    if (mtk_len != KEY_LEN_AES_CCMP || peer_mgtk_len != KEY_LEN_AES_CCMP) {
      return;
    }

    if (peer_igtk && peer_igtk_len != KEY_LEN_AES_CMAC) {
      return;
    }
  }

  auto nlHandler = Nl80211Handler::globalNlHandler;
  auto& netif = nlHandler->lookupMeshNetif();

  VLOG(1) << folly::sformat(
      "Successfully established link with {} on phy{}",
      peer.toString(),
      netif.phyIndex());

  if (!cand->in_kernel) {
    info_elems elems = {};
    elems.sup_rates = cand->sup_rates;
    elems.sup_rates_len = cand->sup_rates_len;

    elems.ht_cap = (unsigned char*)cand->ht_cap;
    elems.ht_cap_len = sizeof(*cand->ht_cap);

    elems.ht_info = (unsigned char*)cand->ht_info;
    elems.ht_info_len = sizeof(*cand->ht_info);

    elems.vht_cap = (unsigned char*)cand->vht_cap;
    elems.vht_cap_len = sizeof(*cand->vht_cap);

    elems.vht_info = (unsigned char*)cand->vht_info;
    elems.vht_info_len = sizeof(*cand->vht_info);

    nlHandler->createUnauthenticatedStation(netif, peer, elems);
    cand->in_kernel = true;
  }

  // Report that we are now authenticated
  nlHandler->setStationAuthenticated(netif, peer);

  if (meshd_conf->is_secure) {
    // Key to encrypt/decrypt unicast data AND mgmt traffic to/from this peer
    nlHandler->installKey(
        netif, peer, Constants::CIPHER_CCMP, NL80211_KEYTYPE_PAIRWISE, 0, mtk);

    // Key to decrypt multicast data traffic from this peer
    nlHandler->installKey(
        netif,
        peer,
        Constants::CIPHER_CCMP,
        NL80211_KEYTYPE_GROUP,
        1,
        peer_mgtk);

    // To check integrity of multicast mgmt frames from this peer
    if (peer_igtk) {
      nlHandler->installKey(
          netif,
          peer,
          Constants::CIPHER_AES_CMAC,
          NL80211_KEYTYPE_GROUP,
          peer_igtk_keyid,
          peer_igtk);
    }
  }
}

static int
add_input(int fd, void* /* data*/, fdcb /* proc */) {
  VLOG(1) << folly::sformat("authsae: {}(fd: {})", __func__, fd);

  // TODO 2018-05-21: This is not yet implemented. It will forward into the
  // Nl80211Handler in an upcoming diff. We use an assert instead of exceptions
  // to be safe because this is called from C code.
  assert(false);

  return 0;
}

static void
rem_input(int fd) {
  VLOG(1) << folly::sformat("authsae: {}(fd: {})", __func__, fd);

  // TODO 2018-05-21: This is not yet implemented. It will forward into the
  // Nl80211Handler in an upcoming diff. We use an assert instead of exceptions
  // to be safe because this is called from C code.
  assert(false);
}

static timerid
add_timeout_with_jitter(
    microseconds usec, timercb proc, void* data, microseconds jitter_usecs) {
  VLOG(1) << folly::sformat(
      "authsae: {}(usec: {}, jitter_usecs: {})", __func__, usec, jitter_usecs);

  // Our timer only has msec resolution
  long long jitter_msecs = jitter_usecs / 1000;
  long long msec = usec / 1000;

  // Calculate jitter using similar random logic to authsae
  if (jitter_msecs != 0) {
    long long rand_number;
    RAND_pseudo_bytes((unsigned char*)&rand_number, sizeof(rand_number));
    long long delta = -jitter_msecs / 2 + llabs(rand_number) % jitter_msecs;
    // Ensure timeout is in the future
    msec = std::max(msec + delta, 1ll);
  }

  // Add the requested callback to the event loop
  int64_t timerId = AuthsaeCallbackHelpers::addTimeoutToEventLoop(
      std::chrono::milliseconds(msec), [proc, data]() { (*proc)(data); });

  // In authsae, timerid 0 is reserved, and addTimeoutToEventLoop() does not
  // return it - enforce this to be extra safer.
  assert(timerId != 0);

  VLOG(1) << folly::sformat("Added timer (timerId {})", timerId);
  return timerId;
}

static timerid
add_timeout(microseconds usec, timercb proc, void* data) {
  VLOG(1) << folly::sformat("authsae: {}(usec: {})", __func__, usec);
  return add_timeout_with_jitter(usec, proc, data, 0);
}

static int
rem_timeout(timerid id) {
  VLOG(1) << folly::sformat("authsae: {}(timerid: {})", __func__, id);

  if (id == 0) {
    // Per authsae's service.h, 0 is an invalid timerid, and trying to remove
    // that timerid should be a no-op
    VLOG(1) << folly::sformat("Not removing invalid timer (timerId {})", id);
    return 0;
  }

  AuthsaeCallbackHelpers::removeTimeoutFromEventLoop(id);
  VLOG(1) << folly::sformat("Removed timer (timerId {})", id);
  return id;
}

static evl_ops*
get_evl_ops() {
  static evl_ops ops;
  ops.add_timeout_with_jitter = add_timeout_with_jitter;
  ops.add_timeout = add_timeout;
  ops.rem_timeout = rem_timeout;
  ops.add_input = add_input;
  ops.rem_input = rem_input;
  return &ops;
}

ampe_cb*
getAmpeCallbacks() {
  static ampe_cb cb;
  cb.meshd_write_mgmt = meshd_write_mgmt;
  cb.meshd_set_mesh_conf = meshd_set_mesh_conf;
  cb.set_plink_state = set_plink_state;
  cb.estab_peer_link = estab_peer_link;
  cb.delete_peer = delete_peer_by_addr;
  cb.evl = get_evl_ops();
  return &cb;
}

sae_cb*
getSaeCallbacks() {
  static sae_cb cb;
  cb.meshd_write_mgmt = meshd_write_mgmt;
  cb.peer_created = peer_created;
  cb.fin = fin;
  cb.evl = get_evl_ops();
  return &cb;
}

// Initialize the AuthsaeCallbackHelpers static class. In particular, store the
// event loop pointer for use by other functions.
void
AuthsaeCallbackHelpers::init(fbzmq::ZmqEventLoop& zmqLoop) {
  VLOG(1) << folly::sformat("AuthsaeCallbackHelpers::{}()", __func__);
  AuthsaeCallbackHelpers::zmqLoop_ = &zmqLoop;
}

// Add a timeout to the event loop. Returns the timeoutId that can be used to
// remove the timeout if needed.
int64_t
AuthsaeCallbackHelpers::addTimeoutToEventLoop(
    std::chrono::milliseconds timeout, fbzmq::TimeoutCallback callback) {
  VLOG(1) << folly::sformat(
      "AuthsaeCallbackHelpers::{}(timeout: {}ms)", __func__, timeout.count());

  CHECK_NOTNULL(zmqLoop_);

  // timeout ID 0 is reserved by authsae, but is a valid return value from zmq.
  // In order to work around this, we abstract the internal zmq timeout ID from
  // the public-facing timeout ID by using the formula:
  //     publicTimeoutId = (privateTimeoutId + 1)
  int64_t timeoutId =
      zmqLoop_->scheduleTimeout(timeout, std::move(callback)) + 1;
  assert(timeoutId != 0);
  return timeoutId;
}

// Remove an existing timeout from the event loop
void
AuthsaeCallbackHelpers::removeTimeoutFromEventLoop(int64_t timeoutId) {
  VLOG(1) << folly::sformat(
      "AuthsaeCallbackHelpers::{}(timeoutId: {})", __func__, timeoutId);

  CHECK_NOTNULL(zmqLoop_);

  // timeout ID 0 is reserved by authsae, but is a valid return value from zmq.
  // In order to work around this, we abstract the internal zmq timeout ID from
  // the public-facing timeout ID by using the formula:
  //     publicTimeoutId = (privateTimeoutId + 1)
  assert(timeoutId != 0);
  zmqLoop_->cancelTimeout(timeoutId - 1);
}
