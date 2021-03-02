#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import time
import unittest

from openr.cli.utils.utils import find_adj_list_deltas, parse_prefix_database
from openr.Network import ttypes as network_types
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork


class UtilsTests(unittest.TestCase):
    @staticmethod
    def create_adjacency(
        otherNodeName,  # : str
        ifName,  # : str
        metric=1,  # : int
        adjLabel=0,  # : int
        isOverloaded=False,  # : bool
        rtt=1,  # : int
        timestamp=0,  # : int
        weight=1,  # : int
        otherIfName="",  # : str
    ):  # -> openr_types.Adjacency
        adj = openr_types.Adjacency(
            otherNodeName=otherNodeName,
            ifName=ifName,
            metric=metric,
            adjLabel=adjLabel,
            isOverloaded=isOverloaded,
            rtt=rtt,
            timestamp=(timestamp if timestamp else int(time.time())),
            weight=weight,
            otherIfName=otherIfName,
        )
        return adj

    def test_find_adj_list_deltas(self):
        adjs_old = [
            self.create_adjacency("nodeA", "ifaceX", metric=10),
            self.create_adjacency("nodeA", "ifaceY", metric=10),
            self.create_adjacency("nodeB", "ifaceX", metric=10),
            self.create_adjacency("nodeC", "ifaceX", metric=10),
        ]

        adjs_new = [
            self.create_adjacency("nodeA", "ifaceX", metric=10),
            self.create_adjacency("nodeB", "ifaceX", metric=20),
            self.create_adjacency("nodeD", "ifaceX", metric=10),
        ]

        delta_list = find_adj_list_deltas(adjs_old, adjs_new)
        self.assertEqual(4, len(delta_list))
        d1, d2, d3, d4 = delta_list

        self.assertEqual(("NEIGHBOR_DOWN", adjs_old[1], None), d1)
        self.assertEqual(("NEIGHBOR_DOWN", adjs_old[3], None), d2)
        self.assertEqual(("NEIGHBOR_UP", None, adjs_new[2]), d3)
        self.assertEqual(("NEIGHBOR_UPDATE", adjs_old[2], adjs_new[1]), d4)

    def test_parse_prefix_database(self):
        bgp1 = openr_types.PrefixEntry(
            prefix=ipnetwork.ip_str_to_prefix("1.0.0.0/8"),
            type=network_types.PrefixType.BGP,
        )
        bgp2 = openr_types.PrefixEntry(
            prefix=ipnetwork.ip_str_to_prefix("2.0.0.0/8"),
            type=network_types.PrefixType.BGP,
        )
        loop1 = openr_types.PrefixEntry(
            prefix=ipnetwork.ip_str_to_prefix("10.0.0.1/32"),
            type=network_types.PrefixType.LOOPBACK,
        )
        prefix_db = openr_types.PrefixDatabase(
            thisNodeName="node1",
            prefixEntries=[bgp1, bgp2, loop1],
            deletePrefix=False,
            perfEvents=None,
        )

        # No filter and ensure we receive everything back
        data = {}
        parse_prefix_database("", "", data, prefix_db)
        self.assertEqual(data["node1"].prefixEntries, [bgp1, bgp2, loop1])

        # Filter on prefix
        data = {}
        parse_prefix_database("10.0.0.1/32", "", data, prefix_db)
        self.assertEqual(data["node1"].prefixEntries, [loop1])

        # Filter on type
        data = {}
        parse_prefix_database("", "bgp", data, prefix_db)
        self.assertEqual(data["node1"].prefixEntries, [bgp1, bgp2])

        # Filter on prefix and type both
        data = {}
        parse_prefix_database("2.0.0.0/8", "bgp", data, prefix_db)
        self.assertEqual(data["node1"].prefixEntries, [bgp2])
