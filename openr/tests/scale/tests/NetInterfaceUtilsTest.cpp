/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <netinet/in.h>

#include <gtest/gtest.h>

#include <openr/tests/scale/NetInterfaceUtils.h>

namespace openr {

namespace {

// Build an in6_addr from a fe80:: link-local with the given bytes 11 and 12
// (the EUI-64 marker bytes are 0xff 0xfe).
struct in6_addr
makeAddr(uint8_t byte11, uint8_t byte12) {
  struct in6_addr addr{};
  addr.s6_addr[0] = 0xfe;
  addr.s6_addr[1] = 0x80;
  addr.s6_addr[11] = byte11;
  addr.s6_addr[12] = byte12;
  return addr;
}

} // namespace

TEST(NetInterfaceUtilsTest, Eui64LinkLocalDetectedByFffeMarker) {
  EXPECT_TRUE(isEui64LinkLocal(makeAddr(0xff, 0xfe)));
}

TEST(NetInterfaceUtilsTest, ConfiguredAddressIsNotEui64) {
  // A manually-configured link-local (e.g. fe80::1) lacks the 0xff 0xfe marker.
  EXPECT_FALSE(isEui64LinkLocal(makeAddr(0x00, 0x01)));
  EXPECT_FALSE(isEui64LinkLocal(makeAddr(0xff, 0x00)));
  EXPECT_FALSE(isEui64LinkLocal(makeAddr(0x00, 0xfe)));
}

TEST(NetInterfaceUtilsTest, Ipv4FromVlanIfNameMapsSuffixToOctets) {
  // vlanId 3 -> 10.(3/256).(3%256).1 = 10.0.3.1
  EXPECT_EQ(ipv4FromVlanIfName("eth0.3"), "10.0.3.1");
}

TEST(NetInterfaceUtilsTest, Ipv4FromVlanIfNameHandlesLargeVlanId) {
  // vlanId 300 -> 10.(300/256).(300%256).1 = 10.1.44.1
  EXPECT_EQ(ipv4FromVlanIfName("eth0.300"), "10.1.44.1");
}

TEST(NetInterfaceUtilsTest, Ipv4FromVlanIfNameUsesLastDotSegment) {
  // Only the final dotted segment is the VLAN id.
  EXPECT_EQ(ipv4FromVlanIfName("eth1.2.5"), "10.0.5.1");
}

TEST(NetInterfaceUtilsTest, Ipv4FromVlanIfNameNoSuffixReturnsZero) {
  EXPECT_EQ(ipv4FromVlanIfName("eth0"), "0.0.0.0");
}

TEST(NetInterfaceUtilsTest, Ipv4FromVlanIfNameNonNumericSuffixReturnsZero) {
  EXPECT_EQ(ipv4FromVlanIfName("eth0.vlan"), "0.0.0.0");
}

} // namespace openr
