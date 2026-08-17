# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import unittest

from openr.tests.scale.scripts.verify_adjacencies import (
    check_count,
    check_ctrl_ports,
    check_distribution,
    check_linkmonitor,
    CheckFailure,
    expected_distribution,
    expected_neighbor_count,
)
from openr.thrift.Types.thrift_types import SparkNeighbor
from parameterized import parameterized


def _neighbors(placement: dict[str, int]) -> list[SparkNeighbor]:
    """Build ESTABLISHED neighbors from {ifName: count}, unique ctrl ports."""
    out: list[SparkNeighbor] = []
    port = 16300
    for if_name, count in placement.items():
        for i in range(count):
            out.append(
                SparkNeighbor(
                    nodeName=f"{if_name}-nbr-{i}",
                    state="ESTABLISHED",
                    localIfName=if_name,
                    openrCtrlThriftPort=port,
                )
            )
            port += 1
    return out


class ExpectedNeighborCountTest(unittest.TestCase):
    @parameterized.expand(
        [
            # (name, role, spines, leaves, super_spines, sites, areas, expected)
            ("leaf_64_spines_with_sites", "leaf", 64, 252, 0, 20, 1, 65),
            ("leaf_no_sites", "leaf", 64, 252, 0, 0, 1, 64),
            ("leaf_smoke", "leaf", 4, 8, 0, 0, 1, 4),
            # numSites only ever contributes the single eb-site-0 node.
            ("leaf_one_site", "leaf", 4, 8, 0, 1, 1, 5),
            ("spine_role_counts_leaves", "spine", 64, 252, 0, 20, 1, 252),
            ("spine_role_adds_super_spines", "spine", 64, 252, 8, 0, 1, 260),
            ("multi_area_multiplies", "leaf", 64, 252, 0, 20, 2, 130),
            ("zero_areas_treated_as_one", "leaf", 4, 8, 0, 0, 0, 4),
        ]
    )
    def test_expected_neighbor_count(
        self,
        _name: str,
        role: str,
        spines: int,
        leaves: int,
        super_spines: int,
        sites: int,
        areas: int,
        expected: int,
    ) -> None:
        self.assertEqual(
            expected,
            expected_neighbor_count(role, spines, leaves, super_spines, sites, areas),
        )


class ExpectedDistributionTest(unittest.TestCase):
    @parameterized.expand(
        [
            ("even_split", 4, 2, [2, 2]),
            # The LAST interface absorbs the remainder — not round-robin.
            ("remainder_on_last", 5, 2, [2, 3]),
            ("scale_65_over_8", 65, 8, [8, 8, 8, 8, 8, 8, 8, 9]),
            ("skewed_65_over_3", 65, 3, [21, 21, 23]),
            ("one_per_interface", 4, 4, [1, 1, 1, 1]),
            # More interfaces than neighbors: leading ones get zero.
            ("more_interfaces_than_neighbors", 1, 3, [0, 0, 1]),
            ("single_interface", 65, 1, [65]),
        ]
    )
    def test_expected_distribution(
        self, _name: str, num_neighbors: int, num_interfaces: int, expected: list[int]
    ) -> None:
        self.assertEqual(expected, expected_distribution(num_neighbors, num_interfaces))

    def test_distribution_always_sums_to_total(self) -> None:
        self.assertEqual(65, sum(expected_distribution(65, 8)))

    def test_zero_interfaces_raises(self) -> None:
        with self.assertRaisesRegex(ValueError, "must be positive"):
            expected_distribution(4, 0)


class CheckCountTest(unittest.TestCase):
    def test_matching_count_passes(self) -> None:
        detail = check_count(_neighbors({"po100111.100": 4}), 4)
        self.assertIn("4 ESTABLISHED", detail)

    def test_short_count_fails(self) -> None:
        with self.assertRaisesRegex(CheckFailure, "3 ESTABLISHED.*expected 4"):
            check_count(_neighbors({"po100111.100": 3}), 4)

    def test_zero_neighbors_fails(self) -> None:
        # The regex-not-in-/etc/openr_config case: DUT ignores the sub-interfaces.
        with self.assertRaisesRegex(CheckFailure, "0 ESTABLISHED"):
            check_count([], 65)


class CheckDistributionTest(unittest.TestCase):
    def test_expected_packing_passes(self) -> None:
        placement = {f"po100111.{100 + i}": 8 for i in range(7)}
        placement["po100111.107"] = 9
        detail = check_distribution(_neighbors(placement), 8)
        self.assertIn("8 interfaces", detail)

    def test_round_robin_packing_fails(self) -> None:
        # 65 spread evenly-ish by round-robin gives 9,9,8,8,8,8,8,7 — not the
        # contiguous-block shape SparkNeighborDistribution produces.
        placement = {"if0": 9, "if1": 9, "if2": 8, "if3": 8}
        placement.update({"if4": 8, "if5": 8, "if6": 8, "if7": 7})
        with self.assertRaisesRegex(CheckFailure, "distribution"):
            check_distribution(_neighbors(placement), 8)

    def test_idle_interface_counted_as_zero(self) -> None:
        # Interfaces with no neighbors never appear in getNeighbors(); they must
        # still be compared as zeros rather than shrinking the observed list.
        detail = check_distribution(_neighbors({"if2": 1}), 3)
        self.assertIn("[0, 0, 1]", detail)

    def test_all_on_wrong_interface_fails(self) -> None:
        with self.assertRaisesRegex(CheckFailure, "distribution"):
            check_distribution(_neighbors({"if0": 4}), 4)


class CheckCtrlPortsTest(unittest.TestCase):
    def test_distinct_ports_pass(self) -> None:
        detail = check_ctrl_ports(_neighbors({"if0": 3}))
        self.assertIn("3 distinct", detail)

    def test_duplicate_ports_fail(self) -> None:
        neighbors = [
            SparkNeighbor(
                nodeName=f"nbr-{i}",
                state="ESTABLISHED",
                localIfName="if0",
                openrCtrlThriftPort=16300,
            )
            for i in range(2)
        ]
        with self.assertRaisesRegex(CheckFailure, "duplicate openrCtrlThriftPort"):
            check_ctrl_ports(neighbors)


class CheckLinkMonitorTest(unittest.TestCase):
    def test_agreeing_views_pass(self) -> None:
        detail = check_linkmonitor(4, _neighbors({"if0": 4}))
        self.assertIn("4 adjacencies", detail)

    def test_disagreeing_views_fail(self) -> None:
        with self.assertRaisesRegex(CheckFailure, "LinkMonitor reports 2"):
            check_linkmonitor(2, _neighbors({"if0": 4}))
