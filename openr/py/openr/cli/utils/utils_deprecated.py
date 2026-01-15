#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import ipaddress
from typing import Union

import click
from openr.Network import ttypes as network_types_py
from openr.OpenrCtrl import ttypes as ctrl_types_py
from openr.py.openr.cli.utils.utils import time_since
from openr.py.openr.utils import ipnetwork, ipnetwork_deprecated, printing
from openr.thrift.Network import thrift_types as network_types
from openr.thrift.OpenrCtrl import thrift_types as ctrl_types
from openr.Types import ttypes as openr_types_py


PrintAdvertisedTypes = Union[
    ctrl_types_py.AdvertisedRoute,
    ctrl_types_py.ReceivedRoute,
    ctrl_types_py.NodeAndArea,
    ctrl_types.AdvertisedRoute,
    ctrl_types.ReceivedRoute,
    ctrl_types.NodeAndArea,
    int,
    network_types_py.PrefixType,
    network_types.PrefixType,
]


def sprint_prefixes_db_full(prefix_db, loopback_only: bool = False) -> str:
    """given serialized prefixes output an array of lines
        representing those prefixes. IPV6 prefixes come before IPV4 prefixes.

    :prefix_db openr_types_py.PrefixDatabase: prefix database
    :loopback_only : is only loopback address expected

    :return [str]: the array of prefix strings
    """

    prefix_strs = []
    sorted_entries = sorted(
        sorted(prefix_db.prefixEntries, key=lambda x: x.prefix.prefixLength),
        key=lambda x: x.prefix.prefixAddress.addr,
    )
    for prefix_entry in sorted_entries:
        if (
            loopback_only
            and prefix_entry.type is not network_types_py.PrefixType.LOOPBACK
        ):
            continue
        prefix_strs.append(
            [
                ipnetwork_deprecated.sprint_prefix(prefix_entry.prefix),
                ipnetwork_deprecated.sprint_prefix_type(prefix_entry.type),
                ipnetwork_deprecated.sprint_prefix_forwarding_type(
                    prefix_entry.forwardingType
                ),
                ipnetwork_deprecated.sprint_prefix_forwarding_algorithm(
                    prefix_entry.forwardingAlgorithm
                ),
            ]
        )

    return printing.render_horizontal_table(
        prefix_strs,
        [
            "Prefix",
            "Client Type",
            "Forwarding Type",
            "Forwarding Algorithm",
        ],
    )


def sprint_adj_db_full(global_adj_db, adj_db, bidir) -> str:
    """given serialized adjacency database, print neighbors. Use the
        global adj database to validate bi-dir adjacencies

    :param global_adj_db map(str, AdjacencyDatabase):
        map of node names to their adjacent node names
    :param adj_db openr_types_py.AdjacencyDatabase: latest from kv store
    :param bidir bool: only print bidir adjacencies

    :return [str]: list of string to be printed
    """

    assert isinstance(adj_db, openr_types_py.AdjacencyDatabase)
    this_node_name = adj_db.thisNodeName

    title_tokens = [this_node_name]
    overload_str = click.style(
        f"{adj_db.isOverloaded}", fg="red" if adj_db.isOverloaded else None
    )
    title_tokens.append(f"Overloaded: {overload_str}")

    column_labels = [
        "Neighbor",
        "Local Intf",
        "Remote Intf",
        "Metric",
        "NextHop-v4",
        "NextHop-v6",
        "Uptime",
    ]

    rows = []
    for adj in adj_db.adjacencies:
        if bidir:
            other_node_db = global_adj_db.get(adj.otherNodeName, None)
            if other_node_db is None:
                continue
            other_node_neighbors = {a.otherNodeName for a in other_node_db.adjacencies}
            if this_node_name not in other_node_neighbors:
                continue

        nh_v6 = ipnetwork.sprint_addr(adj.nextHopV6.addr)
        nh_v4 = ipnetwork.sprint_addr(adj.nextHopV4.addr)
        overload_status = click.style("Overloaded", fg="red")
        metric = overload_status if adj.isOverloaded else adj.metric
        uptime = time_since(adj.timestamp) if adj.timestamp else ""

        rows.append(
            [
                adj.otherNodeName,
                adj.ifName,
                adj.otherIfName,
                metric,
                nh_v4,
                nh_v6,
                uptime,
            ]
        )

    return printing.render_horizontal_table(
        rows, column_labels, caption=", ".join(title_tokens)
    )


def build_unicast_route(
    route: network_types.UnicastRoute | network_types_py.UnicastRoute,
    filter_for_networks: None
    | (list[ipaddress.IPv4Network | ipaddress.IPv6Network]) = None,
    filter_exact_match: bool = False,
    nexthops_to_neighbor_names: dict[bytes, str] | None = None,
) -> tuple[str, list[str]] | None:
    """
    Build unicast route.
        :param route: Unicast Route
        :param filter_for_networks: IP/Prefixes to filter.
        :param filter_exact_match: Indicate exact match or subnet match.
    """

    dest = ipnetwork_deprecated.sprint_prefix(route.dest)
    if filter_for_networks:
        if filter_exact_match:
            if not ipaddress.ip_network(dest) in filter_for_networks:
                return None
        else:
            if not ipnetwork.contain_any_prefix(dest, filter_for_networks):
                return None
    if nexthops_to_neighbor_names:
        nexthops = [
            (
                f"{nexthops_to_neighbor_names[nh.address.addr]}%{nh.address.ifName} "
                + f"[weight {nh.weight if nh.weight != 0 else 1}/metric {nh.metric}]"
            )
            for nh in route.nextHops
        ]
    else:
        nexthops = [ip_nexthop_to_str(nh) for nh in route.nextHops]
    nexthops.sort()
    return dest, nexthops


def ip_nexthop_to_str(
    nextHop: network_types_py.NextHopThrift | network_types.NextHopThrift,
    ignore_v4_iface: bool = False,
    ignore_v6_iface: bool = False,
) -> str:
    """
    Convert Network.BinaryAddress to string representation of a nexthop
    """

    nh = nextHop.address
    ifName = f"%{nh.ifName}" if nh.ifName else ""
    if len(nh.addr) == 4 and ignore_v4_iface:
        ifName = ""
    if len(nh.addr) == 16 and ignore_v6_iface:
        ifName = ""
    addr_str = f"{ipnetwork.sprint_addr(nh.addr)}{ifName}"

    mpls_action_str = " "
    mpls_action = nextHop.mplsAction
    if mpls_action:
        mpls_action_str += mpls_action_to_str(mpls_action)

    # NOTE: Default weight=0 is in-fact weight=1
    weight = f" weight {1 if nextHop.weight == 0 else nextHop.weight}"

    # always put addr_str at head
    # this is consumed by downstream tooling in --json options
    # TODO remove hard dependency on json output format in fbossdeploy/fcr
    return f"{addr_str}{mpls_action_str}{weight}"


def mpls_action_to_str(
    mpls_action: network_types_py.MplsAction | network_types.MplsAction,
) -> str:
    """
    Convert Network.MplsAction to string representation
    """

    if isinstance(mpls_action, network_types_py.MplsAction):
        action_str = network_types_py.MplsActionCode._VALUES_TO_NAMES.get(
            mpls_action.action, ""
        )
    else:
        action_str = mpls_action.action.name

    label_str = ""
    if mpls_action.swapLabel is not None:
        label_str = f" {mpls_action.swapLabel}"
    push_labels = mpls_action.pushLabels
    if push_labels is not None:
        label_str = f" {'/'.join(str(label) for label in push_labels)}"
    return f"mpls {action_str}{label_str}"
