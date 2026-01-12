#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from enum import Enum

from openr.py.openr.utils import ipnetwork
from openr.thrift.Network.thrift_types import IpPrefix, NextHopThrift, UnicastRoute
from openr.thrift.Types.thrift_types import Adjacency, AdjacencyDatabase


class MockedBinaryAddresses(Enum):
    bin_addr1_v4 = ipnetwork.ip_str_to_addr("192.168.21.2", "if_54_2_0")
    bin_addr1_v6 = ipnetwork.ip_str_to_addr("2002::192.168.21.2", "if_54_2_0")
    bin_addr2_v4 = ipnetwork.ip_str_to_addr("192.168.21.9", "if_54_2_1")
    bin_addr2_v6 = ipnetwork.ip_str_to_addr("2002::192.168.21.9", "if_54_2_1")
    bin_addr3_v6 = ipnetwork.ip_str_to_addr("fe80::4f9:99ff:fe8f:f1b3", "if_54_7_0")
    bin_addr4_v6 = ipnetwork.ip_str_to_addr("fe80::3c13:ff:feda:9b6c", "if_54_7_1")
    bin_addr5_noifname_v4 = ipnetwork.ip_str_to_addr("10.10.10.10")
    bin_addr5_noifname_v6 = ipnetwork.ip_str_to_addr("2002::10.10.10.10")
    bin_addr6_noifname_v4 = ipnetwork.ip_str_to_addr("1.1.1.1")
    bin_addr6_noifname_v6 = ipnetwork.ip_str_to_addr("2002::1.1.1.1")
    bin_addr7_noifname_v6 = ipnetwork.ip_str_to_addr("fe80::58:59ff:fef0:62e4")


class NodeNames(Enum):
    node1 = "node1"
    node2 = "node2"
    node3 = "node3"
    node4 = "node4"
    node5 = "node5"
    node6 = "node6"
    node7 = "node7"


MOCKED_ADJDB = AdjacencyDatabase(
    adjacencies=[
        Adjacency(
            otherNodeName=NodeNames.node1.value,
            nextHopV4=MockedBinaryAddresses.bin_addr1_v4.value,
            nextHopV6=MockedBinaryAddresses.bin_addr1_v6.value,
        ),
        Adjacency(
            otherNodeName=NodeNames.node2.value,
            nextHopV4=MockedBinaryAddresses.bin_addr2_v4.value,
            nextHopV6=MockedBinaryAddresses.bin_addr2_v6.value,
        ),
        Adjacency(
            otherNodeName=NodeNames.node3.value,
            nextHopV6=MockedBinaryAddresses.bin_addr3_v6.value,
        ),
        Adjacency(
            otherNodeName=NodeNames.node4.value,
            nextHopV6=MockedBinaryAddresses.bin_addr4_v6.value,
        ),
        Adjacency(
            otherNodeName=NodeNames.node5.value,
            nextHopV4=MockedBinaryAddresses.bin_addr5_noifname_v4.value,
            nextHopV6=MockedBinaryAddresses.bin_addr5_noifname_v6.value,
        ),
        Adjacency(
            otherNodeName=NodeNames.node6.value,
            nextHopV4=MockedBinaryAddresses.bin_addr6_noifname_v4.value,
            nextHopV6=MockedBinaryAddresses.bin_addr6_noifname_v6.value,
        ),
        Adjacency(
            otherNodeName=NodeNames.node7.value,
            nextHopV6=MockedBinaryAddresses.bin_addr7_noifname_v6.value,
        ),
    ]
)

MOCKED_UNICAST_ROUTELIST = [
    UnicastRoute(
        dest=IpPrefix(
            prefixAddress=MockedBinaryAddresses.bin_addr1_v4.value, prefixLength=24
        ),
        nextHops=[
            NextHopThrift(address=MockedBinaryAddresses.bin_addr2_v4.value, weight=1),
            NextHopThrift(address=MockedBinaryAddresses.bin_addr3_v6.value, weight=1),
        ],
    )
]

MOCKED_UNICAST_ROUTELIST_MULTIPLE = [
    UnicastRoute(
        dest=IpPrefix(
            prefixAddress=MockedBinaryAddresses.bin_addr1_v4.value, prefixLength=24
        ),
        nextHops=[
            NextHopThrift(address=MockedBinaryAddresses.bin_addr3_v6.value, weight=1),
        ],
    ),
    UnicastRoute(
        dest=IpPrefix(
            prefixAddress=MockedBinaryAddresses.bin_addr5_noifname_v4.value,
            prefixLength=32,
        ),
        nextHops=[
            NextHopThrift(address=MockedBinaryAddresses.bin_addr2_v4.value, weight=2),
            NextHopThrift(address=MockedBinaryAddresses.bin_addr4_v6.value, weight=1),
        ],
    ),
    UnicastRoute(
        dest=IpPrefix(
            prefixAddress=MockedBinaryAddresses.bin_addr6_noifname_v4.value,
            prefixLength=8,
        ),
        nextHops=[
            NextHopThrift(
                address=MockedBinaryAddresses.bin_addr7_noifname_v6.value, weight=1
            )
        ],
    ),
]
