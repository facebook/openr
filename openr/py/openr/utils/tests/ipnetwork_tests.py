#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import ipaddress
import socket
import unittest

from openr.py.openr.utils.ipnetwork import (
    contain_any_prefix,
    ip_nexthop_to_nexthop_thrift,
    ip_str_to_addr,
    ip_str_to_prefix,
    ip_to_unicast_route,
    ip_version,
    is_ip_addr,
    is_link_local,
    is_same_subnet,
    is_subnet_of,
    mpls_to_mpls_route,
    routes_to_route_db,
    sprint_addr,
    sprint_prefix,
    sprint_prefix_forwarding_algorithm,
    sprint_prefix_forwarding_type,
    sprint_prefix_type,
)
from openr.thrift.Network.thrift_enums import PrefixType as PrefixTypeThriftEnum
from openr.thrift.Network.thrift_types import BinaryAddress, IpPrefix
from openr.thrift.OpenrConfig.thrift_enums import (
    PrefixForwardingAlgorithm as PrefixForwardingAlgorithmThriftEnum,
    PrefixForwardingType as PrefixForwardingTypeThriftEnum,
)


class TestIpNetwork(unittest.TestCase):
    def test_sprint_addr(self) -> None:
        addr_bytes = socket.inet_pton(socket.AF_INET, "192.168.1.1")
        self.assertEqual(sprint_addr(addr_bytes), "192.168.1.1")

    def test_ip_str_to_addr(self) -> None:
        result = ip_str_to_addr("10.0.0.1", "eth0")
        self.assertIsInstance(result, BinaryAddress)
        self.assertEqual(result.addr, socket.inet_pton(socket.AF_INET, "10.0.0.1"))
        self.assertEqual(result.ifName, "eth0")

    def test_ip_str_to_prefix(self) -> None:
        result = ip_str_to_prefix("192.168.0.0/24")
        self.assertIsInstance(result, IpPrefix)
        self.assertEqual(result.prefixLength, 24)

    def test_sprint_prefix(self) -> None:
        prefix = ip_str_to_prefix("172.16.0.0/16")
        self.assertEqual(sprint_prefix(prefix), "172.16.0.0/16")

    def test_ip_nexthop_to_nexthop_thrift(self) -> None:
        result = ip_nexthop_to_nexthop_thrift("10.0.0.1", "eth0", weight=10, metric=100)
        self.assertEqual(result.weight, 10)
        self.assertEqual(result.metric, 100)

    def test_ip_to_unicast_route(self) -> None:
        nexthops = [ip_nexthop_to_nexthop_thrift("10.0.0.1", "eth0")]
        result = ip_to_unicast_route("192.168.0.0/24", nexthops)
        self.assertEqual(result.dest.prefixLength, 24)
        self.assertEqual(len(result.nextHops), 1)

    def test_mpls_to_mpls_route(self) -> None:
        nexthops = [ip_nexthop_to_nexthop_thrift("10.0.0.1", "eth0")]
        result = mpls_to_mpls_route(16000, nexthops)
        self.assertEqual(result.topLabel, 16000)

    def test_routes_to_route_db(self) -> None:
        result = routes_to_route_db("node1")
        self.assertEqual(result.thisNodeName, "node1")
        self.assertEqual(result.unicastRoutes, [])

    def test_sprint_prefix_type(self) -> None:
        result = sprint_prefix_type(PrefixTypeThriftEnum.BGP)
        self.assertEqual(result, "BGP")

    def test_sprint_prefix_forwarding_type(self) -> None:
        result = sprint_prefix_forwarding_type(PrefixForwardingTypeThriftEnum.IP)
        self.assertEqual(result, "IP")

    def test_sprint_prefix_forwarding_algorithm(self) -> None:
        result = sprint_prefix_forwarding_algorithm(
            PrefixForwardingAlgorithmThriftEnum.SP_ECMP
        )
        self.assertEqual(result, "SP_ECMP")

    def test_ip_version(self) -> None:
        self.assertEqual(ip_version("192.168.1.1"), 4)
        self.assertEqual(ip_version("2001:db8::1"), 6)

    def test_is_same_subnet(self) -> None:
        self.assertTrue(is_same_subnet("192.168.1.10", "192.168.1.20", 24))
        self.assertFalse(is_same_subnet("192.168.1.10", "192.168.2.10", 24))

    def test_is_link_local(self) -> None:
        self.assertTrue(is_link_local("169.254.1.1"))
        self.assertFalse(is_link_local("192.168.1.1"))

    def test_is_subnet_of(self) -> None:
        a = ipaddress.ip_network("10.0.0.0/24")
        b = ipaddress.ip_network("10.0.0.0/16")
        self.assertTrue(is_subnet_of(a, b))

    def test_contain_any_prefix(self) -> None:
        networks = [ipaddress.ip_network("10.0.0.0/16")]
        self.assertTrue(contain_any_prefix("10.0.0.0/24", networks))

    def test_is_ip_addr(self) -> None:
        self.assertTrue(is_ip_addr("192.168.1.1"))
        self.assertFalse(is_ip_addr("not_an_ip"))
