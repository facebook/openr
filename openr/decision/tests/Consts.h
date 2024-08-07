/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>

namespace openr {

inline const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
inline const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
inline const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
inline const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");
inline const auto addr5 = toIpPrefix("::ffff:10.4.4.5/128");
inline const auto addr6 = toIpPrefix("::ffff:10.4.4.6/128");
inline const auto addr1V4 = toIpPrefix("10.1.1.1/32");
inline const auto addr2V4 = toIpPrefix("10.2.2.2/32");
inline const auto addr3V4 = toIpPrefix("10.3.3.3/32");
inline const auto addr4V4 = toIpPrefix("10.4.4.4/32");

inline const auto bgpAddr1 = toIpPrefix("2401:1::10.1.1.1/32");
inline const auto bgpAddr2 = toIpPrefix("2401:2::10.2.2.2/32");
inline const auto bgpAddr3 = toIpPrefix("2401:3::10.3.3.3/32");
inline const auto bgpAddr4 = toIpPrefix("2401:4::10.4.4.4/32");
inline const auto bgpAddr1V4 = toIpPrefix("10.11.1.1/16");
inline const auto bgpAddr2V4 = toIpPrefix("10.22.2.2/16");
inline const auto bgpAddr3V4 = toIpPrefix("10.33.3.3/16");
inline const auto bgpAddr4V4 = toIpPrefix("10.43.4.4/16");

inline const auto prefixDb1 = createPrefixDb("1", {createPrefixEntry(addr1)});
inline const auto prefixDb2 = createPrefixDb("2", {createPrefixEntry(addr2)});
inline const auto prefixDb3 = createPrefixDb("3", {createPrefixEntry(addr3)});
inline const auto prefixDb4 = createPrefixDb("4", {createPrefixEntry(addr4)});
inline const auto prefixDb1V4 =
    createPrefixDb("1", {createPrefixEntry(addr1V4)});
inline const auto prefixDb2V4 =
    createPrefixDb("2", {createPrefixEntry(addr2V4)});
inline const auto prefixDb3V4 =
    createPrefixDb("3", {createPrefixEntry(addr3V4)});
inline const auto prefixDb4V4 =
    createPrefixDb("4", {createPrefixEntry(addr4V4)});

/// R1 -> R2, R3, R4
inline const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
inline const auto adj12OnlyUsedBy2 = createAdjacency(
    "2",
    "1/2",
    "2/1",
    "fe80::2",
    "192.168.0.2",
    10,
    100002,
    Constants::kDefaultAdjWeight,
    true);
inline const auto adj12_1 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 1000021);
inline const auto adj12_2 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 20, 1000022);
inline const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 10, 100003);
inline const auto adj14 =
    createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 10, 100004);
// R2 -> R1, R3, R4
inline const auto adj21 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
inline const auto adj21OnlyUsedBy1 = createAdjacency(
    "1",
    "2/1",
    "1/2",
    "fe80::1",
    "192.168.0.1",
    10,
    100001,
    Constants::kDefaultAdjWeight,
    true);
inline const auto adj23 =
    createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 10, 100003);
inline const auto adj24 =
    createAdjacency("4", "2/4", "4/2", "fe80::4", "192.168.0.4", 10, 100004);
// R3 -> R1, R2, R4
inline const auto adj31 =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 10, 100001);
inline const auto adj31_old =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 10, 1000011);
inline const auto adj32 =
    createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 10, 100002);
inline const auto adj34 =
    createAdjacency("4", "3/4", "4/3", "fe80::4", "192.168.0.4", 10, 100004);
// R4 -> R2, R3
inline const auto adj41 =
    createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 10, 100001);
inline const auto adj42 =
    createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 10, 100002);
inline const auto adj43 =
    createAdjacency("3", "4/3", "3/4", "fe80::3", "192.168.0.3", 10, 100003);
// R5 -> R4
inline const auto adj54 =
    createAdjacency("4", "5/4", "4/5", "fe80::4", "192.168.0.4", 10, 100001);

} // namespace openr
