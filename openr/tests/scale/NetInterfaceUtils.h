/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netinet/in.h>

#include <string>
#include <vector>

namespace openr {

/*
 * Generic host network-interface helpers. Stateless free functions; the
 * resolve/lookup ones wrap syscalls, the others are pure.
 */

/*
 * Resolve an interface name to its ifIndex via if_nametoindex(). Returns -1 on
 * failure (logs the errno).
 */
int resolveIfIndex(const std::string& ifName);

/*
 * EUI-64 (auto-generated) link-local addresses carry 0xff 0xfe at bytes 11-12
 * of the address. Returns true for such addresses.
 */
bool isEui64LinkLocal(const struct in6_addr& addr);

/*
 * Look up all link-local IPv6 addresses on an interface via getifaddrs().
 * Configured (non-EUI-64) addresses are returned first so the caller can prefer
 * them over auto-generated ones.
 */
std::vector<std::string> lookupLinkLocalAddrs(const std::string& ifName);

/*
 * Derive the IPv4 address from a VLAN-suffixed interface name (e.g. "eth0.3"
 * -> "10.0.3.1"), matching setup_vlans.sh's scheme. Returns "0.0.0.0" if the
 * name has no numeric VLAN suffix.
 */
std::string ipv4FromVlanIfName(const std::string& ifName);

} // namespace openr
