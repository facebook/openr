/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NetInterface.h"

#include <netlink/route/link.h> // @manual
#include <unistd.h>

#include <string>

#include <glog/logging.h>

#include <folly/ScopeGuard.h>
#include <folly/init/Init.h>

using namespace openr::fbmeshd;

NetInterface::NetInterface(int phyIndex) : phyIndex_{phyIndex} {
  meshConfig_.conf = &meshdConfig_;
}

NetInterface::NetInterface(NetInterface&& other)
    : maybeIfName{std::move(other.maybeIfName)},
      maybeIfIndex{std::move(other.maybeIfIndex)},
      phyName{std::move(other.phyName)},
      meshId{std::move(other.meshId)},
      frequency{std::move(other.frequency)},
      centerFreq1{std::move(other.centerFreq1)},
      centerFreq2{std::move(other.centerFreq2)},
      channelWidth{std::move(other.channelWidth)},
      isMeshCapable{std::move(other.isMeshCapable)},
      maybeMacAddress{std::move(other.maybeMacAddress)},
      isEncrypted{std::move(other.isEncrypted)},
      encryptionPassword{std::move(other.encryptionPassword)},
      encryptionSaeGroups{std::move(other.encryptionSaeGroups)},
      encryptionDebug{std::move(other.encryptionDebug)},
      maxPeerLinks{std::move(other.maxPeerLinks)},
      maybeRssiThreshold{std::move(other.maybeRssiThreshold)},
      ttl{std::move(other.ttl)},
      elementTtl{std::move(other.elementTtl)},
      phyIndex_{std::move(other.phyIndex_)},
      frequencies_{std::move(other.frequencies_)},
      meshConfig_{std::move(other.meshConfig_)},
      meshdConfig_{std::move(other.meshdConfig_)} {
  meshConfig_.conf = &meshdConfig_;
}

bool
NetInterface::isValid() const {
  auto ifName = folly::get_pointer<std::string>(maybeIfName);
  if (!ifName) {
    return false;
  }
  return isMeshCapable;
}

status_t
NetInterface::bringLinkUp() {
  return setLinkFlags(rtnl_link_str2flags("up"));
}

status_t
NetInterface::bringLinkDown() {
  return setLinkFlags(rtnl_link_str2flags("down"));
}

void
NetInterface::addSupportedFrequency(uint32_t freq) {
  frequencies_.insert(freq);
}

bool
NetInterface::isFrequencySupported(uint32_t freq) const {
  return frequencies_.count(freq) > 0;
}

ieee80211_supported_band* FOLLY_NULLABLE
NetInterface::getSupportedBand(ieee80211_band band) {
  if (band >= IEEE80211_NUM_BANDS) {
    return nullptr;
  }
  return &meshConfig_.bands[band];
}

authsae_mesh_node*
NetInterface::getMeshConfig() {
  return &meshConfig_;
}

const authsae_mesh_node*
NetInterface::getMeshConfig() const {
  return &meshConfig_;
}

int
NetInterface::phyIndex() const {
  return phyIndex_;
}

status_t
NetInterface::setLinkFlags(int flag) {
  rtnl_link *link, *request;
  int err;

  nl_sock* sock;

  if (!maybeIfName) {
    return ERR_IFNAME;
  }

  // Right now this class is only accessing RTNL to bring up and down
  // interfaces, so it does not seem that useful to keep a connected
  // socket for the lifetime of the object. Also, considering that we will not
  // have that many mesh interfaces, sharing a socket between a few interfaces
  // does not seem worth the trouble of tracking it externally.
  // Therefore, we just alloc/dealloc for every call.
  sock = nl_socket_alloc();
  if (!sock) {
    LOG(ERROR) << "Failed to allocate rtnetlink socket";
    return ERR_RTNETLINK_ALLOCATE_SOCKET;
  }
  SCOPE_EXIT {
    nl_socket_free(sock);
  };

  nl_connect(sock, NETLINK_ROUTE);

  auto ret = rtnl_link_get_kernel(sock, 0, maybeIfName->c_str(), &link);
  if (ret < 0) {
    LOG(ERROR) << "RTNL error: " << nl_geterror(ret);
    return ERR_IFINDEX_NOT_FOUND;
  }
  SCOPE_EXIT {
    rtnl_link_put(link);
  };

  maybeIfIndex = rtnl_link_get_ifindex(link);

  request = rtnl_link_alloc();
  if (request == nullptr) {
    LOG(ERROR) << "Could not allocate rtnl link object";
    return ERR_RTNL_ALLOCATE_OBJ;
  }
  SCOPE_EXIT {
    rtnl_link_put(request);
  };
  rtnl_link_set_flags(request, flag);

  if ((err = rtnl_link_change(sock, link, request, 0)) < 0) {
    LOG(ERROR) << "Failed to change link state in kernel (" +
            std::to_string(err) + ")";
    return ERR_CHANGE_LINKSTATE;
  }

  nl_close(sock);
  return R_SUCCESS;
}

std::ostream&
openr::fbmeshd::operator<<(std::ostream& os, const NetInterface& netIntf) {
  os << "phyIndex: " << netIntf.phyIndex_ << std::endl;
  os << "phyName: " << netIntf.phyName << std::endl;
  os << "meshId: " << netIntf.meshId << std::endl;
  os << "frequency: " << netIntf.frequency << std::endl;
  if (netIntf.maybeIfName) {
    os << "ifName: " << *netIntf.maybeIfName << std::endl;
  }
  if (netIntf.maybeIfIndex) {
    os << "ifIndex: " << std::to_string(netIntf.maybeIfIndex.value())
       << std::endl;
  }
  os << "mesh capable: " << std::boolalpha << netIntf.isMeshCapable
     << std::endl;
  os << "supported frequencies: " << netIntf.frequencies_.size() << std::endl;
  if (netIntf.maybeRssiThreshold) {
    os << "rssi threshold: " << *netIntf.maybeRssiThreshold << std::endl;
  }
  os << "ttl: " << netIntf.ttl << std::endl;
  os << "element ttl: " << netIntf.elementTtl << std::endl;
  if (netIntf.maybeMacAddress) {
    os << "mac address: " << *netIntf.maybeMacAddress << std::endl;
  }
  os << "max peer links: " << netIntf.maxPeerLinks << std::endl;
  return os;
}
