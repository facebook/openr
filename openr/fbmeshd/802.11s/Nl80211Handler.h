/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "nl80211-copy.h"

#include <netlink/netlink.h> // @manual

#include <chrono>
#include <map>
#include <memory>
#include <vector>

#include <fbzmq/async/ZmqEventLoop.h>
#include <folly/Optional.h>
#include <folly/Portability.h>

#include <openr/fbmeshd/802.11s/NetInterface.h>
#include <openr/fbmeshd/common/ErrorCodes.h>
#include <openr/fbmeshd/if/gen-cpp2/fbmeshd_types.h>
#include <openr/fbmeshd/nl/GenericNetlinkMessage.h>
#include <openr/fbmeshd/nl/GenericNetlinkSocket.h>
#include <openr/fbmeshd/nl/TabularNetlinkAttribute.h>

// Used in main.cpp
DECLARE_string(mesh_ifname);

// Also used in unit tests
DECLARE_string(encryption_sae_groups);
DECLARE_string(mesh_channel_type);
DECLARE_int32(mesh_frequency);
DECLARE_string(mesh_id);
DECLARE_string(mesh_mac_address);
DECLARE_uint32(mesh_max_peer_links);
DECLARE_int32(mesh_rssi_threshold);
DECLARE_uint32(mesh_ttl);
DECLARE_uint32(mesh_ttl_element);
DECLARE_uint32(mesh_hwmp_active_path_timeout);
DECLARE_uint32(mesh_hwmp_rann_interval);

namespace openr {
namespace fbmeshd {

struct StationInfo {
  folly::MacAddress macAddress;
  std::chrono::milliseconds inactiveTime;
  int32_t signalAvgDbm;
  bool isConnectedToGate;
  uint32_t expectedThroughput;
};

class PeerSelector;

class Nl80211HandlerInterface {
 public:
  virtual ~Nl80211HandlerInterface() {}
  FOLLY_NODISCARD virtual std::vector<folly::MacAddress> getPeers() = 0;
  FOLLY_NODISCARD virtual std::vector<StationInfo> getStationsInfo() = 0;
  virtual void setRssiThreshold(int32_t rssiThreshold) = 0;
  virtual void setPeerSelector(PeerSelector* peerSelector) = 0;
  virtual void deleteStation(folly::MacAddress peer) = 0;
};

// The class to handle communication with the kernel's netlink 80211 system
class Nl80211Handler final : public Nl80211HandlerInterface {
  // This class should never be copied; remove default copy/move
  Nl80211Handler() = delete;
  Nl80211Handler(const Nl80211Handler&) = delete;
  Nl80211Handler(Nl80211Handler&&) = delete;
  Nl80211Handler& operator=(const Nl80211Handler&) = delete;
  Nl80211Handler& operator=(Nl80211Handler&&) = delete;

 public:
  Nl80211Handler(fbzmq::ZmqEventLoop& zmqLoop, bool userspace_mesh_peering);

  ~Nl80211Handler();

  // Count of how many net interfaces can do mesh
  FOLLY_NODISCARD int getNumberOfMeshPhys();

  // Bring up all mesh interfaces and connect them to their configured meshes
  status_t joinMeshes();

  // Bring down all mesh interfaces and leave their configured meshes
  status_t leaveMeshes();

  // Register to receive frames from the kernel for userspace processing
  void registerForAuthFrames(const NetInterface& netif);
  void registerForMeshPeeringFrames(const NetInterface& netif);

  // Install a key to the wireless driver
  void installKey(
      const NetInterface& netif,
      folly::Optional<folly::MacAddress> maybePeer,
      unsigned int cipher,
      unsigned int keytype,
      unsigned char keyidx,
      unsigned char* keydata);

  // Transmit an arbitrary frame of data on an interface
  status_t txFrame(const NetInterface& netif, folly::ByteRange frame);

  // Return the NetInterface object for the given phy
  FOLLY_NODISCARD NetInterface& lookupNetifFromPhy(int phyIndex);

  // Returns NetInterface object that is being used for meshing (as only one
  // interface may be used for meshing at a time)
  //
  // Note: this is intended for use when a mesh interface already exists; if no
  // encrypted interface is found, a std::runtime_error is thrown.
  FOLLY_NODISCARD NetInterface& lookupMeshNetif();

  friend std::ostream& operator<<(std::ostream& out, const Nl80211Handler& nl);

  FOLLY_NODISCARD std::vector<folly::MacAddress> getPeers() override;

  FOLLY_NODISCARD std::vector<StationInfo> getStationsInfo() override;

  FOLLY_NODISCARD thrift::Mesh getMesh();

  FOLLY_NODISCARD std::unordered_map<folly::MacAddress, int32_t> getMetrics();

  // authsae has callbacks into named C functions that forward the calls to this
  // class to actually do the netlink operations that are necessary. These
  // methods must be public as a result, although they are not intended for the
  // public to call.
  void createUnauthenticatedStation(
      const NetInterface& netif,
      folly::MacAddress peer,
      const info_elems& elems);
  void deleteStation(folly::MacAddress peer) override;
  void setStationAuthenticated(
      const NetInterface& netif, folly::MacAddress peer);

  void setPlinkState(
      const NetInterface& netif, folly::MacAddress peer, int state);
  status_t setMeshConfig(
      const NetInterface& netif,
      const authsae_mesh_node& mesh,
      unsigned int changed);
  void setMeshConnectedToGate(bool isConnected);
  void setRootMode(uint8_t mode);
  void setRssiThreshold(int32_t rssiThreshold) override;

  void setPeerSelector(PeerSelector* peerSelector) override;
  static std::vector<NetInterface> populateNetifs();

 private:
  // Configuration methods
  static void printConfiguration();
  static void validateConfiguration();

  // Initialization/cleanup methods
  void initNlSockets();
  status_t handleNewCandidate(const GenericNetlinkMessage& msg);
  status_t handleDeletedPeer(const GenericNetlinkMessage& msg);

  static void parseWiphyBands(
      NetInterface& netInterface,
      const TabularNetlinkAttribute<NL80211_ATTR_MAX>& tb_msg);
  void tearDown();

  void processResponse(
      std::function<int(const GenericNetlinkMessage&)>& valid_cb);
  inline void
  processResponse(std::function<int(const GenericNetlinkMessage&)>&& valid_cb) {
    processResponse(valid_cb);
  }
  static void processResponse(
      std::function<int(const GenericNetlinkMessage&)>& valid_cb,
      const GenericNetlinkSocket&);
  static inline void
  processResponse(
      std::function<int(const GenericNetlinkMessage&)>&& valid_cb,
      const GenericNetlinkSocket& sock) {
    processResponse(valid_cb, sock);
  }
  void processResponse();

  void eventDataReady();

  // Network interface control methods
  folly::MacAddress createInterface(int phyIndex);
  void deleteInterface(const std::string& ifName);

  void getNl80211MulticastGroups();
  void applyConfiguration();
  status_t processEvent(const GenericNetlinkMessage& msg);

  // Methods for interacting with meshes
  status_t initMesh(int phyIndex);
  void joinMesh(int phyIndex);
  void leaveMesh(int phyIndex);

 public:
  // authsae has callbacks into named C functions that we must implement. As we
  // do all netlink operations using this class, we keep track of the existing
  // Nl80211Handler and use the required static C functions to forward calls to
  // it. This is initialised in initMesh() when an encrypted mesh is first
  // created.
  static Nl80211Handler* globalNlHandler;

 private:
  GenericNetlinkSocket nlEventSocket_;
  std::map<uint32_t, NetInterface> netInterfaces_;
  std::map<std::string, uint32_t> multicastGroups_;
  fbzmq::ZmqEventLoop& zmqLoop_;
  std::unordered_map<folly::MacAddress, int32_t> metrics_;
  // peer selector that is notified if peer membership changes
  PeerSelector* peerSelector_{nullptr};
  bool userspace_mesh_peering_;
};

} // namespace fbmeshd
} // namespace openr
