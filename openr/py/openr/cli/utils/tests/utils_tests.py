#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import time
import unittest

from openr.cli.utils.utils import find_adj_list_deltas
from openr.Lsdb import ttypes as lsdb_types


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
    ):  # -> lsdb_types.Adjacency
        adj = lsdb_types.Adjacency(
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
