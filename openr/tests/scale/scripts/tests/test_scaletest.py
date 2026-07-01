# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from __future__ import annotations

import unittest
from typing import Any

from openr.tests.scale.scripts.scaletest import _build_config


def _base_doc() -> dict[str, Any]:
    """A minimal, valid YAML-equivalent mapping accepted by _build_config."""
    return {
        "dut": {"host": "dut.example", "port": 2018},
        "topology": {
            "dutRole": "leaf",
            "numSpines": 4,
            "numLeaves": 8,
            "numSuperSpines": 0,
            "numPods": 2,
            "numSites": 0,
            "numPrefixesPerNode": 1,
            "ecmpWidth": 2,
        },
        "injection": {
            "injectTopology": True,
            "simulateNeighbors": False,
            "enableFakeKvStore": False,
        },
    }


class BuildConfigAreasTest(unittest.TestCase):
    def test_areas_absent_yields_none(self) -> None:
        # No "areas" key -> field stays unset so the daemon keeps single-area
        # behavior (default area "0"), distinct from an explicit empty list.
        cfg = _build_config(_base_doc())
        self.assertIsNone(cfg.topology.areas)

    def test_multi_area_passed_through_in_order(self) -> None:
        doc = _base_doc()
        doc["topology"]["areas"] = ["pod", "plane"]
        cfg = _build_config(doc)
        self.assertEqual(list(cfg.topology.areas or []), ["pod", "plane"])

    def test_single_area_passed_through(self) -> None:
        doc = _base_doc()
        doc["topology"]["areas"] = ["solo"]
        cfg = _build_config(doc)
        self.assertEqual(list(cfg.topology.areas or []), ["solo"])

    def test_areas_non_list_raises(self) -> None:
        # A bare scalar would become list("pod") == ["p", "o", "d"]; the builder
        # must reject it instead of silently fanning out per-character areas.
        doc = _base_doc()
        doc["topology"]["areas"] = "pod"
        with self.assertRaisesRegex(ValueError, "areas must be a list"):
            _build_config(doc)
