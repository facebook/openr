# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Off-box mirror of the scale tester's KvStore key names.

Computes the exact ``adj:`` and ``prefix:`` keys the injector will publish, from
the topology flags alone. That is what lets a test assert which keys are present
by name instead of only counting them: at 64 spines / 256 leaves, 3,729 of the
4,068 keys are ``prefix:`` keys whose addresses were previously drawn from
unseeded ``folly::Random`` and so were unknowable off-box.

Nothing here observes the injector -- no shared state, no readback. Both sides
compute independently from ``(seed, nodeName, prefixIndex)``, which is only
sound because every input is a topology parameter. Anything run-varying (time,
hostname, iteration order) would make the two silently disagree.

The C++ side is ``openr/tests/scale/DeterministicPrefixGenerator.{h,cpp}``, and
both are pinned to the same golden vectors:
``openr/tests/scale/tests/testdata/seeded_prefix_golden.json``. Whichever side
drifts fails its own test.
"""

from __future__ import annotations

import hashlib
import ipaddress
import typing as t

# openr/common/Constants.h: kAdjDbMarker, kPrefixDbMarker.
ADJ_KEY_MARKER = "adj:"
PREFIX_KEY_MARKER = "prefix:"

# TopologyGenerator.cpp: kScalePrefixMaskLen. Scale-tester prefixes are host
# routes, and the masked address is what lands in the key name.
SCALE_PREFIX_MASK_LEN = 128

# DeterministicPrefixGenerator.cpp: kUlaFirstByte. Keeps derived addresses in
# fc00::/8, matching PrefixGenerator's convention.
_ULA_FIRST_BYTE = 0xFC


def derive_address(seed: int, node_name: str, index: int) -> ipaddress.IPv6Address:
    """The unmasked address for one ``(seed, node_name, index)``. 0-based index.

    Deliberately a hash of a canonical string rather than a seeded RNG stream:
    mirroring ``folly::Random`` here would mean staying bug-compatible with it
    forever. The hashed bytes are the cross-language contract, so the format
    string below must not change without changing the C++ side and the golden
    vectors together.
    """
    digest = hashlib.sha256(f"{seed}|{node_name}|{index}".encode()).digest()
    return ipaddress.IPv6Address(bytes([_ULA_FIRST_BYTE]) + digest[:15])


def prefix_key_name(
    seed: int,
    node_name: str,
    index: int,
    bit_mask_len: int = SCALE_PREFIX_MASK_LEN,
) -> str:
    """The ``prefix:`` key the injector will publish for one prefix.

    Mirrors ``PrefixKey``'s V2 layout (openr/common/LsdbTypes.cpp), which is
    ``prefix:<node>:[<cidr>]``. Note the area is NOT part of the key, so the
    same node in two areas yields the same key name in each.
    """
    network = ipaddress.IPv6Network(
        (derive_address(seed, node_name, index), bit_mask_len), strict=False
    )
    return f"{PREFIX_KEY_MARKER}{node_name}:[{network.network_address}/{bit_mask_len}]"


def prefix_key_names(
    seed: int,
    node_name: str,
    num_prefixes: int,
    bit_mask_len: int = SCALE_PREFIX_MASK_LEN,
) -> t.List[str]:
    """Every ``prefix:`` key for one node, in the injector's index order."""
    return [
        prefix_key_name(seed, node_name, index, bit_mask_len)
        for index in range(num_prefixes)
    ]


def adj_key_name(node_name: str) -> str:
    """The ``adj:`` key the injector will publish for one node."""
    return f"{ADJ_KEY_MARKER}{node_name}"


def bbf_simple_node_names(
    num_spines: int,
    num_leaves: int,
    num_control_nodes: int,
    num_sites: int,
    dut_role: str,
) -> t.List[str]:
    """Synthetic node names a ``bbf-simple`` topology will inject.

    Mirrors ``BbfTopologyGenerator::createBbfSimple``, including its creation
    order, because the prefix index restarts at 0 per node and a reordering
    would therefore not change any key name -- but a rename would.

    The DUT replaces ``<dut_role>-0``, so that node is injected by nobody and
    must be excluded or it will be reported as a missing node.
    """
    names = [
        *(f"spine-{index}" for index in range(num_spines)),
        *(f"leaf-{index}" for index in range(num_leaves)),
        *(f"control-{index}" for index in range(num_control_nodes)),
        *(f"eb-site-{index}" for index in range(num_sites)),
    ]
    replaced_by_dut = f"{dut_role}-0"
    return [name for name in names if name != replaced_by_dut]


def expected_key_set(
    node_names: t.Sequence[str],
    seed: int,
    prefixes_per_node: int,
    bit_mask_len: int = SCALE_PREFIX_MASK_LEN,
) -> t.Set[str]:
    """Every KvStore key the injector will publish, by exact name.

    A set, not a list: KvStore is keyed by name, so re-injecting the same seed
    overwrites rather than accumulating, and a duplicate would be invisible on
    the device anyway.
    """
    keys = set()
    for node_name in node_names:
        keys.add(adj_key_name(node_name))
        keys.update(prefix_key_names(seed, node_name, prefixes_per_node, bit_mask_len))
    return keys
