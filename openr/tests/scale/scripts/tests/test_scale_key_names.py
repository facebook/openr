# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import importlib.resources
import ipaddress
import json
import typing as t
import unittest

from openr.tests.scale.scripts.scale_key_names import (
    adj_key_name,
    bbf_simple_node_names,
    derive_address,
    expected_key_set,
    prefix_key_name,
    prefix_key_names,
    SCALE_PREFIX_MASK_LEN,
)

# Anchored at the package that owns the JSON. Traversable.joinpath rejects "..",
# so this cannot be reached from the scale_key_names module's own package.
_GOLDEN_PACKAGE = "openr.tests.scale"
_GOLDEN_RESOURCE = "tests/testdata/seeded_prefix_golden.json"

# The topology the scale runs use, from the injection step's defaults.
_PRODUCTION_NUM_SPINES = 64
_PRODUCTION_NUM_LEAVES = 256
_PRODUCTION_NUM_CONTROL_NODES = 0
_PRODUCTION_NUM_SITES = 20
_PRODUCTION_PREFIXES_PER_NODE = 11
_PRODUCTION_DUT_ROLE = "leaf"


def _load_golden() -> t.Dict[str, t.Any]:
    return json.loads(
        importlib.resources.files(_GOLDEN_PACKAGE)
        .joinpath(_GOLDEN_RESOURCE)
        .read_text(encoding="utf-8")
    )


class ScaleKeyNamesTest(unittest.TestCase):
    def test_reproduces_golden_derivation_vectors(self) -> None:
        """Same file the C++ generator asserts, so drift on either side fails.

        The seed arrives as a decimal string, which is what lets the uint64
        maximum survive JSON parsers that would otherwise coerce it to a float.
        """
        derivation = _load_golden()["derivation"]
        self.assertTrue(derivation)

        for entry in derivation:
            seed = int(entry["seed"])
            with self.subTest(
                seed=seed,
                node=entry["node"],
                index=entry["index"],
                why=entry["why"],
            ):
                key = prefix_key_name(
                    seed, entry["node"], entry["index"], entry["bitMaskLen"]
                )
                self.assertEqual(entry["key"], key)

    def test_formats_addresses_per_rfc5952(self) -> None:
        """Zero-run compression is the one place inet_ntop and ipaddress could
        disagree, and a disagreement would make every derived key name differ.
        """
        formatting = _load_golden()["addressFormatting"]
        self.assertTrue(formatting)

        for entry in formatting:
            with self.subTest(bytes_hex=entry["bytesHex"], why=entry["why"]):
                address = ipaddress.IPv6Address(bytes.fromhex(entry["bytesHex"]))
                self.assertEqual(entry["address"], str(address))

    def test_derived_addresses_are_unique_local(self) -> None:
        for index in range(256):
            address = derive_address(42, "leaf-0", index)
            self.assertEqual(0xFC, address.packed[0], f"index={index}")

    def test_seed_and_node_both_change_the_address(self) -> None:
        """Both must be part of the derivation: without the node name every node
        in the fabric would advertise an identical prefix set.
        """
        baseline = derive_address(7, "leaf-3", 0)
        self.assertNotEqual(baseline, derive_address(8, "leaf-3", 0))
        self.assertNotEqual(baseline, derive_address(7, "spine-3", 0))
        self.assertNotEqual(baseline, derive_address(7, "leaf-3", 1))

    def test_prefix_key_names_start_at_index_zero(self) -> None:
        names = prefix_key_names(7, "leaf-3", 3)
        self.assertEqual(
            [prefix_key_name(7, "leaf-3", index) for index in range(3)], names
        )

    def test_bbf_simple_node_names_mirror_creation_order(self) -> None:
        """Order matters only for review against createBbfSimple; the key names
        depend on the node name, not its position.
        """
        names = bbf_simple_node_names(
            num_spines=2,
            num_leaves=3,
            num_control_nodes=1,
            num_sites=2,
            dut_role="leaf",
        )
        self.assertEqual(
            [
                "spine-0",
                "spine-1",
                "leaf-1",
                "leaf-2",
                "control-0",
                "eb-site-0",
                "eb-site-1",
            ],
            names,
        )

    def test_bbf_simple_node_names_excludes_the_dut(self) -> None:
        """The DUT replaces <dut_role>-0, so nobody injects that node's keys and
        including it would report a spurious missing node.
        """

        self.assertNotIn(
            "leaf-0",
            bbf_simple_node_names(
                num_spines=1,
                num_leaves=2,
                num_control_nodes=0,
                num_sites=0,
                dut_role="leaf",
            ),
        )
        self.assertNotIn(
            "spine-0",
            bbf_simple_node_names(
                num_spines=2,
                num_leaves=1,
                num_control_nodes=0,
                num_sites=0,
                dut_role="spine",
            ),
        )

    def test_production_topology_node_count(self) -> None:
        names = bbf_simple_node_names(
            num_spines=_PRODUCTION_NUM_SPINES,
            num_leaves=_PRODUCTION_NUM_LEAVES,
            num_control_nodes=_PRODUCTION_NUM_CONTROL_NODES,
            num_sites=_PRODUCTION_NUM_SITES,
            dut_role=_PRODUCTION_DUT_ROLE,
        )
        # 64 spines + 256 leaves + 20 sites, less the one the DUT replaces.
        self.assertEqual(339, len(names))
        self.assertEqual(len(names), len(set(names)))

    def test_production_expected_key_set_size(self) -> None:
        """339 nodes x (1 adj: + 11 prefix:) = 4,068 keys, of which 3,729 are the
        prefix keys that were previously unknowable off-box.
        """
        names = bbf_simple_node_names(
            num_spines=_PRODUCTION_NUM_SPINES,
            num_leaves=_PRODUCTION_NUM_LEAVES,
            num_control_nodes=_PRODUCTION_NUM_CONTROL_NODES,
            num_sites=_PRODUCTION_NUM_SITES,
            dut_role=_PRODUCTION_DUT_ROLE,
        )
        keys = expected_key_set(
            names, seed=12345, prefixes_per_node=_PRODUCTION_PREFIXES_PER_NODE
        )

        self.assertEqual(4068, len(keys))
        prefix_keys = {key for key in keys if key.startswith("prefix:")}
        self.assertEqual(3729, len(prefix_keys))
        self.assertEqual(339, len(keys) - len(prefix_keys))

    def test_expected_key_set_has_no_collisions_across_nodes(self) -> None:
        """A collision would silently shrink the expected set and weaken the
        assertion, so the count is checked rather than just set membership.
        """
        names = bbf_simple_node_names(
            num_spines=8,
            num_leaves=16,
            num_control_nodes=2,
            num_sites=4,
            dut_role="leaf",
        )
        keys = expected_key_set(names, seed=12345, prefixes_per_node=11)
        self.assertEqual(len(names) * 12, len(keys))

    def test_expected_key_set_is_stable_across_calls(self) -> None:
        names = bbf_simple_node_names(
            num_spines=2,
            num_leaves=4,
            num_control_nodes=0,
            num_sites=0,
            dut_role="leaf",
        )
        self.assertEqual(
            expected_key_set(names, seed=12345, prefixes_per_node=3),
            expected_key_set(names, seed=12345, prefixes_per_node=3),
        )

    def test_expected_key_set_changes_with_seed(self) -> None:
        names = ["leaf-1"]
        self.assertNotEqual(
            expected_key_set(names, seed=12345, prefixes_per_node=3),
            expected_key_set(names, seed=54321, prefixes_per_node=3),
        )

    def test_adj_key_name(self) -> None:
        self.assertEqual("adj:leaf-1", adj_key_name("leaf-1"))

    def test_prefix_key_layout_matches_prefixkey_v2(self) -> None:
        """prefix:<node>:[<cidr>] -- the area is deliberately absent, matching
        PrefixKey's V2 layout, so the same node in two areas yields one name.
        """
        key = prefix_key_name(12345, "leaf-0", 0)
        self.assertTrue(key.startswith("prefix:leaf-0:["))
        self.assertTrue(key.endswith(f"/{SCALE_PREFIX_MASK_LEN}]"))

        address = key[len("prefix:leaf-0:[") : -len(f"/{SCALE_PREFIX_MASK_LEN}]")]
        self.assertEqual(
            derive_address(12345, "leaf-0", 0), ipaddress.IPv6Address(address)
        )

    def test_masking_is_applied(self) -> None:
        self.assertEqual(
            "prefix:leaf-0:[fc71:b00f:4932:2e27::/64]",
            prefix_key_name(12345, "leaf-0", 0, 64),
        )
