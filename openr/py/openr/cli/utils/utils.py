#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import copy
import curses
import datetime
import ipaddress
import json
import re
import sys
from builtins import chr, input, map
from collections import defaultdict
from functools import lru_cache, partial
from itertools import product
from typing import Any, Dict, List, Optional, Set, Tuple, Union, Callable, Sequence

import bunch
import click
from openr.clients.openr_client import get_openr_ctrl_client
from openr.Network import ttypes as network_types
from openr.OpenrConfig import ttypes as config_types
from openr.OpenrCtrl import OpenrCtrl, ttypes as ctrl_types
from openr.Platform import FibService, ttypes as platform_types
from openr.thrift.Network import types as network_types_py3
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing
from openr.utils.consts import Consts
from openr.utils.serializer import deserialize_thrift_object
from thrift.protocol import TBinaryProtocol
from thrift.transport import TSocket, TTransport


PrintAdvertisedTypes = Union[
    ctrl_types.AdvertisedRoute,
    ctrl_types.ReceivedRoute,
    ctrl_types.NodeAndArea,
    network_types.PrefixType,
]


def yesno(question, skip_confirm=False):
    """
    Ask a yes/no question. No default, we want to avoid mistakes as
    much as possible. Repeat the question until we receive a valid
    answer.
    """

    if skip_confirm:
        print("Skipping interactive confirmation!")
        return True

    while True:
        try:
            prompt = "{} [yn] ".format(question)
            answer = input(prompt).lower()
        except EOFError:
            with open("/dev/tty") as sys.stdin:
                continue
        if answer in ["y", "yes"]:
            return True
        elif answer in ["n", "no"]:
            return False


def json_dumps(data):
    """
    Gives consistent formatting for JSON dumps for our CLI

    :param data: python dictionary object

    :return: json encoded string
    """

    def make_serializable(obj):
        """
        Funtion called if a non seralizable object is hit
        - Today we only support bytes to str for Python 3

        :param obj: object that can not be serializable

        :return: decode of bytes to a str
        """

        return obj.decode("utf-8")

    return json.dumps(
        data, default=make_serializable, sort_keys=True, indent=2, ensure_ascii=False
    )


def time_since(timestamp):
    """
    :param timestamp: in seconds since unix time

    :returns: difference between now and the timestamp, in a human-friendly,
              condensed format

    Example format:

    time_since(10000)

    :rtype: datetime.timedelta
    """
    time_since_epoch = datetime.datetime.utcnow() - datetime.datetime(
        year=1970, month=1, day=1
    )
    tdelta = time_since_epoch - datetime.timedelta(seconds=timestamp)
    d = {"days": tdelta.days}
    d["hours"], rem = divmod(tdelta.seconds, 3600)
    d["minutes"], d["seconds"] = divmod(rem, 60)
    if d["days"]:
        fmt = "{days}d{hours}h"
    elif d["hours"]:
        fmt = "{hours}h{minutes}m"
    else:
        fmt = "{minutes}m{seconds}s"
    return fmt.format(**d)


def get_fib_agent_client(
    host, port, timeout_ms, client_id=platform_types.FibClient.OPENR, service=FibService
):
    """
    Get thrift client for talking to Fib thrift service

    :param host: thrift server name or ip
    :param port: thrift server port

    :returns: The thrift client
    :rtype: FibService.Client
    """
    transport = TSocket.TSocket(host, port)
    transport.setTimeout(timeout_ms)
    transport = TTransport.TFramedTransport(transport)
    protocol = TBinaryProtocol.TBinaryProtocol(transport)
    client = service.Client(protocol)
    client.host = host  # Assign so that we can refer later on
    client.port = port  # Assign so that we can refer later on
    client.client_id = client_id  # Assign so that we can refer later on
    transport.open()
    return client


def parse_nodes(cli_opts, nodes):
    """parse nodes from user input

    :return set: the set of nodes
    """

    if not nodes:
        with get_openr_ctrl_client(cli_opts.host, cli_opts) as client:
            nodes = client.getMyNodeName()
    nodes = set(nodes.strip().split(","))

    return nodes


def sprint_prefixes_db_full(prefix_db, loopback_only=False):
    """given serialized prefixes output an array of lines
        representing those prefixes. IPV6 prefixes come before IPV4 prefixes.

    :prefix_db openr_types.PrefixDatabase: prefix database
    :loopback_only : is only loopback address expected

    :return [str]: the array of prefix strings
    """

    prefix_strs = []
    sorted_entries = sorted(
        sorted(prefix_db.prefixEntries, key=lambda x: x.prefix.prefixLength),
        key=lambda x: x.prefix.prefixAddress.addr,
    )
    for prefix_entry in sorted_entries:
        if loopback_only and prefix_entry.type is not network_types.PrefixType.LOOPBACK:
            continue
        prefix_strs.append(
            [
                ipnetwork.sprint_prefix(prefix_entry.prefix),
                ipnetwork.sprint_prefix_type(prefix_entry.type),
                ipnetwork.sprint_prefix_forwarding_type(prefix_entry.forwardingType),
                ipnetwork.sprint_prefix_forwarding_algorithm(
                    prefix_entry.forwardingAlgorithm
                ),
                str(prefix_entry.prependLabel) if prefix_entry.prependLabel else "",
            ]
        )

    return printing.render_horizontal_table(
        prefix_strs,
        [
            "Prefix",
            "Client Type",
            "Forwarding Type",
            "Forwarding Algorithm",
            "Prepend Label",
        ],
    )


def alloc_prefix_to_loopback_ip_str(prefix):
    """
    :param prefix: IpPrefix representing an allocation prefix (CIDR network)

    :returns: Loopback IP corresponding to allocation prefix
    :rtype: string
    """

    ip_addr = prefix.prefixAddress.addr
    print(ip_addr)
    if prefix.prefixLength != 128:
        ip_addr = ip_addr[:-1] + chr(ord(ip_addr[-1]) | 1)
    print(ip_addr)
    return ipnetwork.sprint_addr(ip_addr)


def parse_prefix_database(
    prefix_filter: Optional[Union[network_types.IpPrefix, str]],
    client_type_filter: Optional[Union[network_types.PrefixType, str]],
    prefix_dbs: Dict[str, openr_types.PrefixDatabase],
    prefix_db: Any,
):
    """
    Utility function to prase `prefix_db` with filter and populate prefix_dbs
    accordingly
    """
    if client_type_filter:
        _TYPES = network_types.PrefixType._NAMES_TO_VALUES
        if isinstance(client_type_filter, str):
            client_type_filter = _TYPES.get(client_type_filter.upper(), None)
            if client_type_filter is None:
                raise Exception(
                    f"Unknown client type. Use one of {list(_TYPES.keys())}"
                )

    if prefix_filter:
        if isinstance(prefix_filter, str):
            prefix_filter = ipnetwork.ip_str_to_prefix(prefix_filter)

    if isinstance(prefix_db, openr_types.Value):
        prefix_db = deserialize_thrift_object(
            prefix_db.value, openr_types.PrefixDatabase
        )

    if prefix_db.deletePrefix:
        # In per prefix-key, deletePrefix flag is set to indicate prefix
        # withdrawl
        return

    if prefix_db.thisNodeName not in prefix_dbs:
        prefix_dbs[prefix_db.thisNodeName] = openr_types.PrefixDatabase(
            f"{prefix_db.thisNodeName}", []
        )

    for prefix_entry in prefix_db.prefixEntries:
        if prefix_filter and prefix_filter != prefix_entry.prefix:
            continue
        if client_type_filter and client_type_filter != prefix_entry.type:
            continue
        prefix_dbs[prefix_db.thisNodeName].prefixEntries.append(prefix_entry)


def print_prefixes_table(resp, nodes, prefix, client_type, iter_func):
    """print prefixes"""

    rows = []
    prefix_maps = {}
    iter_func(
        prefix_maps, resp, nodes, partial(parse_prefix_database, prefix, client_type)
    )
    for node_name, prefix_db in prefix_maps.items():
        rows.append(["{}".format(node_name), sprint_prefixes_db_full(prefix_db)])
    print(printing.render_vertical_table(rows))


def thrift_to_dict(thrift_inst, update_func=None):
    """convert thrift instance into a dict in strings

    :param thrift_inst: a thrift instance
    :param update_func: transformation function to update dict value of
                        thrift object. It is optional.

    :return dict: dict with attributes as key, value in strings
    """

    if thrift_inst is None:
        return None

    gen_dict = copy.copy(thrift_inst).__dict__
    if update_func is not None:
        update_func(gen_dict, thrift_inst)

    return gen_dict


def metric_vector_to_dict(metric_vector):
    def _update(metric_vector_dict, metric_vector):
        metric_vector_dict.update(
            {"metrics": [thrift_to_dict(m) for m in metric_vector.metrics]}
        )

    return thrift_to_dict(metric_vector, _update)


def collate_prefix_keys(
    kvstore_keyvals: openr_types.KeyVals,
) -> Dict[str, openr_types.PrefixDatabase]:
    """collate all the prefixes of node and return a map of
    nodename - PrefixDatabase
    """

    prefix_maps = {}
    for key, value in sorted(kvstore_keyvals.items()):
        if key.startswith(Consts.PREFIX_DB_MARKER):

            node_name = key.split(":")[1]
            prefix_db = deserialize_thrift_object(
                value.value, openr_types.PrefixDatabase
            )
            if prefix_db.deletePrefix:
                continue
            if node_name not in prefix_maps:
                prefix_maps[node_name] = openr_types.PrefixDatabase(f"{node_name}", [])

            for prefix_entry in prefix_db.prefixEntries:
                prefix_maps[node_name].prefixEntries.append(prefix_entry)

    return prefix_maps


def prefix_entry_to_dict(prefix_entry):
    """convert prefixEntry from thrift instance into a dict in strings"""

    def _update(prefix_entry_dict, prefix_entry):
        # prefix and data need string conversion and metric_vector can be
        # represented as a dict so we update them
        prefix_entry_dict.update(
            {
                "prefix": ipnetwork.sprint_prefix(prefix_entry.prefix),
                "data": str(prefix_entry.data)
                if prefix_entry.data is not None
                else None,
                "metrics": thrift_to_dict(prefix_entry.metrics),
                "tags": list(prefix_entry.tags if prefix_entry.tags else []),
                "mv": metric_vector_to_dict(prefix_entry.mv)
                if prefix_entry.mv
                else None,
            }
        )

    return thrift_to_dict(prefix_entry, _update)


def prefix_db_to_dict(prefix_db: Any) -> Dict[str, Any]:
    """convert PrefixDatabase from thrift instance to a dictionary"""

    if isinstance(prefix_db, openr_types.Value):
        prefix_db = deserialize_thrift_object(
            prefix_db.value, openr_types.PrefixDatabase
        )

    def _update(prefix_db_dict, prefix_db):
        prefix_db_dict.update(
            {"prefixEntries": list(map(prefix_entry_to_dict, prefix_db.prefixEntries))}
        )

    return thrift_to_dict(prefix_db, _update)


def print_prefixes_json(resp, nodes, prefix, client_type, iter_func):
    """print prefixes in json"""

    prefixes_map = {}
    iter_func(
        prefixes_map, resp, nodes, partial(parse_prefix_database, prefix, client_type)
    )
    for node_name, prefix_db in prefixes_map.items():
        prefixes_map[node_name] = prefix_db_to_dict(prefix_db)
    print(json_dumps(prefixes_map))


def update_global_adj_db(global_adj_db, adj_db):
    """update the global adj map based on publication from single node

    :param global_adj_map map(node, AdjacencyDatabase)
        the map for all adjacencies in the network - to be updated
    :param adj_db openr_types.AdjacencyDatabase: publication from single
        node
    """

    assert isinstance(adj_db, openr_types.AdjacencyDatabase)

    global_adj_db[adj_db.thisNodeName] = adj_db


def build_global_adj_db(resp):
    """build a map of all adjacencies in the network. this is used
    for bi-directional validation

    :param resp openr_types.Publication: the parsed publication

    :return map(node, AdjacencyDatabase): the global
        adj map, devices name mapped to devices it connects to, and
        properties of that connection
    """

    # map: (node) -> AdjacencyDatabase)
    global_adj_db = {}

    for (key, value) in resp.keyVals.items():
        if not key.startswith(Consts.ADJ_DB_MARKER):
            continue
        adj_db = deserialize_thrift_object(value.value, openr_types.AdjacencyDatabase)
        update_global_adj_db(global_adj_db, adj_db)

    return global_adj_db


def build_global_prefix_db(resp):
    """build a map of all prefixes in the network. this is used
    for checking for changes in topology

    :param resp openr_types.Publication: the parsed publication

    :return map(node, set([prefix])): the global prefix map,
        prefixes mapped to the node
    """

    # map: (node) -> set([prefix])
    global_prefix_db = {}
    prefix_maps = collate_prefix_keys(resp.keyVals)

    for _, prefix_db in prefix_maps.items():
        update_global_prefix_db(global_prefix_db, prefix_db)

    return global_prefix_db


def dump_adj_db_full(global_adj_db, adj_db, bidir):
    """given an adjacency database, dump neighbors. Use the
        global adj database to validate bi-dir adjacencies

    :param global_adj_db map(str, AdjacencyDatabase):
        map of node names to their adjacent node names
    :param adj_db openr_types.AdjacencyDatabase: latest from kv store
    :param bidir bool: only dump bidir adjacencies

    :return (nodeLabel, [adjacencies]): tuple of node label and list
        of adjacencies
    """

    assert isinstance(adj_db, openr_types.AdjacencyDatabase)
    this_node_name = adj_db.thisNodeName
    area = adj_db.area if adj_db.area is not None else "N/A"

    if not bidir:
        return (adj_db.nodeLabel, adj_db.isOverloaded, adj_db.adjacencies, area)

    adjacencies = []

    for adj in adj_db.adjacencies:
        other_node_db = global_adj_db.get(adj.otherNodeName, None)
        if other_node_db is None:
            continue
        other_node_neighbors = {
            (a.otherNodeName, a.otherIfName) for a in other_node_db.adjacencies
        }
        if (this_node_name, adj.ifName) not in other_node_neighbors:
            continue
        adjacencies.append(adj)

    return (adj_db.nodeLabel, adj_db.isOverloaded, adjacencies, area)


def adj_db_to_dict(adjs_map, adj_dbs, adj_db, bidir, version):
    """convert adj db to dict"""

    node_label, is_overloaded, adjacencies, area = dump_adj_db_full(
        adj_dbs, adj_db, bidir
    )

    if not adjacencies:
        return

    def adj_to_dict(adj):
        """convert adjacency from thrift instance into a dict in strings"""

        def _update(adj_dict, adj):
            # Only addrs need string conversion so we udpate them
            adj_dict.update(
                {
                    "nextHopV6": ipnetwork.sprint_addr(adj.nextHopV6.addr),
                    "nextHopV4": ipnetwork.sprint_addr(adj.nextHopV4.addr),
                    "area": area,
                }
            )

        return thrift_to_dict(adj, _update)

    adjacencies = list(map(adj_to_dict, adjacencies))

    # Dump is keyed by node name with attrs as key values
    adjs_map[adj_db.thisNodeName] = {
        "node_label": node_label,
        "overloaded": is_overloaded,
        "adjacencies": adjacencies,
        "area": area,
    }
    if version:
        adjs_map[adj_db.thisNodeName]["version"] = version


def adj_dbs_to_dict(resp, nodes, bidir, iter_func):
    """get parsed adjacency db

    :param resp openr_types.Publication, or decision_types.adjDbs
    :param nodes set: the set of the nodes to print prefixes for
    :param bidir bool: only dump bidirectional adjacencies

    :return map(node, map(adjacency_keys, (adjacency_values)): the parsed
        adjacency DB in a map with keys and values in strings
    """
    adj_dbs = resp
    if isinstance(adj_dbs, openr_types.Publication):
        adj_dbs = build_global_adj_db(resp)

    def _parse_adj(adjs_map, adj_db):
        version = None
        if isinstance(adj_db, openr_types.Value):
            version = adj_db.version
            adj_db = deserialize_thrift_object(
                adj_db.value, openr_types.AdjacencyDatabase
            )
        adj_db_to_dict(adjs_map, adj_dbs, adj_db, bidir, version)

    adjs_map = {}
    iter_func(adjs_map, resp, nodes, _parse_adj)
    return adjs_map


def adj_dbs_to_area_dict(
    resp: List[openr_types.AdjacencyDatabase],
    nodes: Set[str],
    bidir: bool,
):
    """get parsed adjacency db for all areas as a two level mapping:
    {area: {node: adjDb}}

    :param resp: resp from server of type decision_types.adjDbs
    :param nodes set: the set of the nodes to print prefixes for
    :param bidir bool: only dump bidirectional adjacencies

    :return map(area, map(node, map(adjacency_keys, (adjacency_values))):
        the parsed adjacency DB in a map with keys and values in strings
    """
    adj_dbs = resp
    version = None

    adj_dbs_dict_all_areas = {}

    # run through the adjDb List, and create a two level dict:
    # {area: {node: adjDb}}
    for db in adj_dbs:
        if db.area not in adj_dbs_dict_all_areas:
            adj_dbs_dict_all_areas[db.area] = {}
        adj_dbs_dict_all_areas[db.area][db.thisNodeName] = db

    # run through each area, filter on node, and run each
    # per-node adjDb through adj_db_to_dict()
    for area, adj_dbs_dict_per_area in sorted(adj_dbs_dict_all_areas.items()):
        adjs_map = {}
        for node, adj_db_per_node in sorted(adj_dbs_dict_per_area.items()):
            if "all" not in nodes and node not in nodes:
                continue
            adj_db_to_dict(
                adjs_map, adj_dbs_dict_per_area, adj_db_per_node, bidir, version
            )
        adj_dbs_dict_all_areas[area] = adjs_map

    # return the two level dict back
    return adj_dbs_dict_all_areas


def print_json(map, file=sys.stdout):
    """
    Print object in json format. Use this function for consistent json style
    formatting for output. Further it prints to `stdout` stream.

    @map: object that needs to be printed in json
    """

    print(json_dumps(map), file=file)


def print_adjs_table(adjs_map, neigh=None, interface=None):
    """print adjacencies

    :param adjacencies as list of dict
    """

    column_labels = [
        "Neighbor",
        "Local Intf",
        "Remote Intf",
        "Metric",
        "Label",
        "NextHop-v4",
        "NextHop-v6",
        "Uptime",
        "Area",
    ]

    output = []
    adj_found = False
    for node, val in sorted(adjs_map.items()):
        adj_tokens = []

        # report adjacency version
        if "version" in val:
            adj_tokens.append("Version: {}".format(val["version"]))

        # report overloaded only when it is overloaded
        is_overloaded = val["overloaded"]
        if is_overloaded:
            overload_str = "{}".format(is_overloaded)
            if is_color_output_supported():
                overload_str = click.style(overload_str, fg="red")
            adj_tokens.append("Overloaded: {}".format(overload_str))

        # report node label if non zero
        node_label = val["node_label"]
        if node_label:
            adj_tokens.append("Node Label: {}".format(node_label))

        # horizontal adj table for a node
        rows = []
        seg = ""
        for adj in sorted(val["adjacencies"], key=lambda adj: adj["otherNodeName"]):
            # filter if set
            if neigh is not None and interface is not None:
                if neigh == adj["otherNodeName"] and interface == adj["ifName"]:
                    adj_found = True
                else:
                    continue

            overload_status = click.style("Overloaded", fg="red")
            metric = (
                (overload_status if is_color_output_supported() else "OVERLOADED")
                if adj["isOverloaded"]
                else adj["metric"]
            )
            uptime = time_since(adj["timestamp"]) if adj["timestamp"] else ""
            area = (
                adj["area"]
                if "area" in adj.keys() and adj["area"] is not None
                else "N/A"
            )

            rows.append(
                [
                    adj["otherNodeName"],
                    adj["ifName"],
                    adj["otherIfName"],
                    metric,
                    adj["adjLabel"],
                    adj["nextHopV4"],
                    adj["nextHopV6"],
                    uptime,
                    area,
                ]
            )
            seg = printing.render_horizontal_table(
                rows, column_labels, tablefmt="plain"
            )
        cap = "{} {} {}".format(node, "=>" if adj_tokens else "", ", ".join(adj_tokens))
        output.append([cap, seg])

    if neigh is not None and interface is not None and not adj_found:
        print("Adjacency with {} {} is not formed.".format(neigh, interface))
        return

    print(printing.render_vertical_table(output))


def sprint_adj_db_full(global_adj_db, adj_db, bidir):
    """given serialized adjacency database, print neighbors. Use the
        global adj database to validate bi-dir adjacencies

    :param global_adj_db map(str, AdjacencyDatabase):
        map of node names to their adjacent node names
    :param adj_db openr_types.AdjacencyDatabase: latest from kv store
    :param bidir bool: only print bidir adjacencies

    :return [str]: list of string to be printed
    """

    assert isinstance(adj_db, openr_types.AdjacencyDatabase)
    this_node_name = adj_db.thisNodeName

    title_tokens = [this_node_name]
    overload_str = click.style(
        f"{adj_db.isOverloaded}", fg="red" if adj_db.isOverloaded else None
    )
    title_tokens.append("Overloaded: {}".format(overload_str))
    if adj_db.nodeLabel:
        title_tokens.append(f"Node Label: {adj_db.nodeLabel}")

    column_labels = [
        "Neighbor",
        "Local Intf",
        "Remote Intf",
        "Metric",
        "Label",
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
                adj.adjLabel,
                nh_v4,
                nh_v6,
                uptime,
            ]
        )

    return printing.render_horizontal_table(
        rows, column_labels, caption=", ".join(title_tokens)
    )


def interface_db_to_dict(value):
    """
    Convert a thrift::Value representation of InterfaceDatabase to bunch
    object
    """

    def _parse_intf_info(info):
        addrs = [ipnetwork.sprint_addr(v.prefixAddress.addr) for v in info.networks]

        return bunch.Bunch(
            **{"isUp": info.isUp, "ifIndex": info.ifIndex, "Addrs": addrs}
        )

    assert isinstance(value, openr_types.Value)
    intf_db = deserialize_thrift_object(value.value, openr_types.InterfaceDatabase)
    return bunch.Bunch(
        **{
            "thisNodeName": intf_db.thisNodeName,
            "interfaces": {
                k: _parse_intf_info(v) for k, v in intf_db.interfaces.items()
            },
        }
    )


def interface_dbs_to_dict(publication, nodes, iter_func):
    """get parsed interface dbs

    :param publication openr_types.Publication
    :param nodes set: the set of the nodes to filter interfaces for

    :return map(node, InterfaceDatabase.bunch): the parsed
        adjacency DB in a map with keys and values in strings
    """

    assert isinstance(publication, openr_types.Publication)

    def _parse_intf_db(intf_map, value):
        intf_db = interface_db_to_dict(value)
        intf_map[intf_db.thisNodeName] = intf_db

    intf_dbs_map = {}
    iter_func(intf_dbs_map, publication, nodes, _parse_intf_db)
    return intf_dbs_map


def next_hop_thrift_to_dict(nextHop: network_types.NextHopThrift) -> Dict[str, Any]:
    """convert nextHop from thrift instance into a dict in strings"""
    if nextHop is None:
        return {}

    def _update(next_hop_dict, nextHop):
        next_hop_dict.update(
            {
                "address": ipnetwork.sprint_addr(nextHop.address.addr),
                "nextHop": ipnetwork.sprint_addr(nextHop.address.addr),
                "ifName": nextHop.address.ifName,
            }
        )
        if nextHop.mplsAction:
            next_hop_dict.update({"mplsAction": thrift_to_dict(nextHop.mplsAction)})

    return thrift_to_dict(nextHop, _update)


def unicast_route_to_dict(route):
    """convert route from thrift instance into a dict in strings"""

    def _update(route_dict, route):
        route_dict.update(
            {
                "dest": ipnetwork.sprint_prefix(route.dest),
                "nextHops": [next_hop_thrift_to_dict(nh) for nh in route.nextHops],
            }
        )

    return thrift_to_dict(route, _update)


def mpls_route_to_dict(route: network_types.MplsRoute) -> Dict[str, Any]:
    """
    Convert MPLS route to json serializable dict object
    """

    def _update(route_dict, route: network_types.MplsRoute):
        route_dict.update(
            {"nextHops": [next_hop_thrift_to_dict(nh) for nh in route.nextHops]}
        )

    return thrift_to_dict(route, _update)


def route_db_to_dict(route_db: openr_types.RouteDatabase) -> Dict[str, Any]:
    """
    Convert route from thrift instance into a dict in strings
    """

    ret = {
        "unicastRoutes": [unicast_route_to_dict(r) for r in route_db.unicastRoutes],
        "mplsRoutes": [mpls_route_to_dict(r) for r in route_db.mplsRoutes],
    }
    return ret


def print_routes_json(
    route_db_dict,
    prefixes: Optional[List[str]] = None,
    labels: Optional[List[int]] = None,
):
    """
    Print json representation of routes. Takes prefixes and labels to
    filter
    """

    networks = None
    if prefixes:
        networks = [ipaddress.ip_network(p) for p in prefixes]

    # Filter out all routes based on prefixes and labels
    for routes in route_db_dict.values():
        if "unicastRoutes" in routes:
            filtered_unicast_routes = []
            for route in routes["unicastRoutes"]:
                if labels or networks:
                    if networks and ipnetwork.contain_any_prefix(
                        route["dest"], networks
                    ):
                        filtered_unicast_routes.append(route)
                else:
                    filtered_unicast_routes.append(route)
            routes["unicastRoutes"] = filtered_unicast_routes

        if "mplsRoutes" in routes:
            filtered_mpls_routes = []
            for route in routes["mplsRoutes"]:
                if labels or prefixes:
                    if labels and int(route["topLabel"]) in labels:
                        filtered_mpls_routes.append(route)
                else:
                    filtered_mpls_routes.append(route)
            routes["mplsRoutes"] = filtered_mpls_routes

    print(json_dumps(route_db_dict))


def print_route_db(
    route_db: openr_types.RouteDatabase,
    prefixes: Optional[List[str]] = None,
    labels: Optional[List[int]] = None,
) -> None:
    """print the routes from Decision/Fib module"""

    if prefixes or not labels:
        print_unicast_routes(
            "Unicast Routes for {}".format(route_db.thisNodeName),
            route_db.unicastRoutes,
            prefixes=prefixes,
        )
    if labels or not prefixes:
        print_mpls_routes(
            "MPLS Routes for {}".format(route_db.thisNodeName),
            route_db.mplsRoutes,
            labels=labels,
        )


def find_adj_list_deltas(old_adj_list, new_adj_list, tags=None):
    """given the old adj list and the new one for some node, return
    change list.

    :param old_adj_list [Adjacency]: old adjacency list
    :param new_adj_list [Adjacency]: new adjacency list
    :param tags 3-tuple(string): a tuple of labels for
        (in old only, in new only, in both but different)

    :return [(str, Adjacency, Adjacency)]: list of tuples of
        (changeType, oldAdjacency, newAdjacency)
        in the case where an adjacency is added or removed,
        oldAdjacency or newAdjacency is None, respectively
    """
    if not tags:
        tags = ("NEIGHBOR_DOWN", "NEIGHBOR_UP", "NEIGHBOR_UPDATE")

    old_neighbors = {(a.otherNodeName, a.ifName) for a in old_adj_list}
    new_neighbors = {(a.otherNodeName, a.ifName) for a in new_adj_list}
    delta_list = [
        (tags[0], a, None)
        for a in old_adj_list
        if (a.otherNodeName, a.ifName) in old_neighbors - new_neighbors
    ]
    delta_list.extend(
        [
            (tags[1], None, a)
            for a in new_adj_list
            if (a.otherNodeName, a.ifName) in new_neighbors - old_neighbors
        ]
    )
    delta_list.extend(
        [
            (tags[2], a, b)
            for a, b in product(old_adj_list, new_adj_list)
            if (
                a.otherNodeName == b.otherNodeName
                and a.ifName == b.ifName
                and (a.otherNodeName, a.ifName) in new_neighbors & old_neighbors
                and a != b
            )
        ]
    )
    return delta_list


def adj_list_deltas_json(adj_deltas_list, tags=None):
    """
    Parses a list of adjacency list deltas (from func find_adj_list_deltas),
    and returns the data as a json-formatted dict, and a status code.
        {
            tag-down: [nodes_down],
            tag-up: [nodes_up],
            tag-update: [
                {
                    "old_adj": old_adj,
                    "new_adj": new_adj
                }
            ]
        }

    @param adj_deltas_list: list<(changeType, oldAdjacency, newAdjacency)>
    @param tags: 3-tuple(string). a tuple of labels for
        (in old only, in new only, in both but different)
    """
    if not tags:
        tags = "NEIGHBOR_DOWN, NEIGHBOR_UP, NEIGHBOR_UPDATE"

    return_code = 0
    nodes_down = []
    nodes_up = []
    nodes_update = []

    for data in adj_deltas_list:
        old_adj = adjacency_to_dict(data[1]) if data[1] else None
        new_adj = adjacency_to_dict(data[2]) if data[2] else None

        if data[0] == tags[0]:
            assert new_adj is None
            nodes_down.append(old_adj)
            return_code = 1
        elif data[0] == tags[1]:
            assert old_adj is None
            nodes_up.append(new_adj)
            return_code = 1
        elif data[0] == tags[2]:
            assert old_adj is not None and new_adj is not None
            nodes_update.append({tags[0]: old_adj, tags[1]: new_adj})
            return_code = 1
        else:
            raise ValueError(
                'Unexpected change type "{}" in adjacency deltas list'.format(data[0])
            )

    deltas_json = {}

    if nodes_down:
        deltas_json.update({tags[0]: nodes_down})
    if nodes_up:
        deltas_json.update({tags[1]: nodes_up})
    if nodes_update:
        deltas_json.update({tags[2]: nodes_update})

    return deltas_json, return_code


def adjacency_to_dict(adjacency):
    """convert adjacency from thrift instance into a dict in strings

    :param adjacency as a thrift instance: adjacency

    :return dict: dict with adjacency attributes as key, value in strings
    """

    # Only addrs need string conversion so we udpate them
    adj_dict = copy.copy(adjacency).__dict__
    adj_dict.update(
        {
            "nextHopV6": ipnetwork.sprint_addr(adjacency.nextHopV6.addr),
            "nextHopV4": ipnetwork.sprint_addr(adjacency.nextHopV4.addr),
        }
    )

    return adj_dict


def sprint_adj_delta(old_adj, new_adj):
    """given old and new adjacency, create a list of strings that summarize
    changes. If oldAdj is None, this function prints all attridutes of
    newAdj

    :param oldAdj Adjacency: can be None
    :param newAdj Adjacency: new

    :return str: table summarizing the change
    """
    assert new_adj is not None
    rows = []
    new_adj_dict = adjacency_to_dict(new_adj)
    if old_adj is not None:
        old_adj_dict = adjacency_to_dict(old_adj)
        for k in sorted(new_adj_dict.keys()):
            if old_adj_dict.get(k) != new_adj_dict.get(k):
                rows.append([k, old_adj_dict.get(k), "-->", new_adj_dict.get(k)])
    else:
        for k in sorted(new_adj_dict.keys()):
            rows.append([k, new_adj_dict[k]])
    return printing.render_horizontal_table(rows)


def sprint_pub_update(global_publication_db, key, value):
    """
    store new version and originatorId for a key in the global_publication_db
    return a string summarizing any changes in a publication from kv store
    """

    rows = []
    old_value = global_publication_db.get(key, openr_types.Value())

    if old_value.version != value.version:
        rows.append(["version:", old_value.version, "-->", value.version])
    if old_value.originatorId != value.originatorId:
        rows.append(
            ["originatorId:", old_value.originatorId, "-->", value.originatorId]
        )
    if old_value.ttlVersion != value.ttlVersion:
        rows.append(["ttlVersion:", old_value.ttlVersion, "-->", value.ttlVersion])
    if old_value.ttl != value.ttl:
        if not rows:
            print("Unexpected update with value but only ttl change")
        old_ttl = "INF" if old_value.ttl == Consts.CONST_TTL_INF else old_value.ttl
        ttl = "INF" if value.ttl == Consts.CONST_TTL_INF else value.ttl
        rows.append(["ttl:", old_ttl, "-->", ttl])
    global_publication_db[key] = value
    return printing.render_horizontal_table(rows, tablefmt="plain") if rows else ""


def update_global_prefix_db(
    global_prefix_db: Dict, prefix_db: Dict, key: Optional[str] = None
):
    """update the global prefix map with a single publication

    :param global_prefix_map map(node, set([str])): map of all prefixes
        in the network
    :param prefix_db openr_types.PrefixDatabase: publication from single
        node
    """

    assert isinstance(prefix_db, openr_types.PrefixDatabase)

    prefix_set = set()
    for prefix_entry in prefix_db.prefixEntries:
        addr_str = ipnetwork.sprint_addr(prefix_entry.prefix.prefixAddress.addr)
        prefix_len = prefix_entry.prefix.prefixLength
        prefix_set.add("{}/{}".format(addr_str, prefix_len))

    # per prefix key format contains only one key, it can be an 'add' or 'delete'
    if key and re.match(Consts.PER_PREFIX_KEY_REGEX, key):
        node_prefix_set = global_prefix_db.get(prefix_db.thisNodeName, set())
        if prefix_db.deletePrefix:
            node_prefix_set = node_prefix_set - prefix_set
        else:
            node_prefix_set.update(prefix_set)
    else:
        global_prefix_db[prefix_db.thisNodeName] = prefix_set

    return


def sprint_adj_db_delta(new_adj_db, old_adj_db):
    """given serialized adjacency database, print neighbors delta as
        compared to the supplied global state

    :param new_adj_db openr_types.AdjacencyDatabase: latest from kv store
    :param old_adj_db openr_types.AdjacencyDatabase: last one we had

    :return [str]: list of string to be printed
    """

    # check for deltas between old and new
    # first check for changes in the adjacencies lists
    adj_list_deltas = find_adj_list_deltas(
        old_adj_db.adjacencies, new_adj_db.adjacencies
    )

    strs = []

    for change_type, old_adj, new_adj in adj_list_deltas:
        if change_type == "NEIGHBOR_DOWN":
            strs.append(
                "{}: {} via {}".format(
                    change_type, old_adj.otherNodeName, old_adj.ifName
                )
            )
        if change_type == "NEIGHBOR_UP" or change_type == "NEIGHBOR_UPDATE":
            strs.append(
                "{}: {} via {}\n{}".format(
                    change_type,
                    new_adj.otherNodeName,
                    new_adj.ifName,
                    sprint_adj_delta(old_adj, new_adj),
                )
            )

    # check for other adjDB changes
    old_db_dict = copy.copy(old_adj_db).__dict__
    old_db_dict.pop("adjacencies", None)
    old_db_dict.pop("perfEvents", None)
    new_db_dict = copy.copy(new_adj_db).__dict__
    new_db_dict.pop("adjacencies", None)
    new_db_dict.pop("perfEvents", None)
    if new_db_dict != old_db_dict:
        rows = []
        strs.append("ADJ_DB_UPDATE: {}".format(new_adj_db.thisNodeName))
        for k in sorted(new_db_dict.keys()):
            if old_db_dict.get(k) != new_db_dict.get(k):
                rows.append([k, old_db_dict.get(k), "-->", new_db_dict.get(k)])
        strs.append(printing.render_horizontal_table(rows, tablefmt="plain"))

    return strs


def sprint_prefixes_db_delta(
    global_prefixes_db: Dict,
    prefix_db: Dict,
    key: Optional[str] = None,
):
    """given serialzied prefixes for a single node, output the delta
        between those prefixes and global prefixes snapshot

    prefix could be entire prefix DB or per prefix key
    entire prefix DB: prefix:<node name>
    per prefix key: prefix:<node name>:<area>:<[IP addr/prefix len]

    :global_prefixes_db map(node, set([str])): global prefixes
    :prefix_db openr_types.PrefixDatabase: latest from kv store

    :return [str]: the array of prefix strings
    """

    # pyre-fixme[16]: `Dict` has no attribute `thisNodeName`.
    this_node_name = prefix_db.thisNodeName
    prev_prefixes = global_prefixes_db.get(this_node_name, set())

    added_prefixes = set()
    removed_prefixes = set()
    cur_prefixes = set()

    # pyre-fixme[16]: `Dict` has no attribute `prefixEntries`.
    for prefix_entry in prefix_db.prefixEntries:
        cur_prefixes.add(ipnetwork.sprint_prefix(prefix_entry.prefix))

    # per prefix key format contains only one key, it can be an 'add' or 'delete'
    if key and re.match(Consts.PER_PREFIX_KEY_REGEX, key):
        # pyre-fixme[16]: `Dict` has no attribute `deletePrefix`.
        if prefix_db.deletePrefix:
            removed_prefixes = cur_prefixes
        else:
            added_prefixes = cur_prefixes
    else:
        added_prefixes = cur_prefixes - prev_prefixes
        removed_prefixes = prev_prefixes - cur_prefixes

    strs = ["+ {}".format(prefix) for prefix in added_prefixes]
    strs.extend(["- {}".format(prefix) for prefix in removed_prefixes])

    return strs


def dump_node_kvs(
    cli_opts: bunch.Bunch,
    host: str,
    area: Optional[str] = None,
) -> openr_types.Publication:
    pub = None

    with get_openr_ctrl_client(host, cli_opts) as client:
        keyDumpParams = openr_types.KeyDumpParams(Consts.ALL_DB_MARKER)
        keyDumpParams.keys = [Consts.ALL_DB_MARKER]
        if area is None:
            pub = client.getKvStoreKeyValsFiltered(keyDumpParams)
        else:
            pub = client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)

    return pub


def print_allocations_table(alloc_str):
    """print static allocations"""

    rows = []
    allocations = deserialize_thrift_object(alloc_str, openr_types.StaticAllocation)
    for node, prefix in allocations.nodePrefixes.items():
        rows.append([node, ipnetwork.sprint_prefix(prefix)])
    print(printing.render_horizontal_table(rows, ["Node", "Prefix"]))


def build_nexthops(nexthops: List[str]) -> List[network_types.BinaryAddress]:
    """
    Convert nexthops in list of string to list of binaryAddress
    """

    nhs = []
    for nh_iface in nexthops:
        iface, addr = None, None
        # Nexthop may or may not be link-local. Handle it here well
        if "@" in nh_iface:
            addr, iface = nh_iface.split("@")
        elif "%" in nh_iface:
            addr, iface = nh_iface.split("%")
        else:
            addr = nh_iface
        nexthop = ipnetwork.ip_str_to_addr(addr)
        nexthop.ifName = iface
        nhs.append(nexthop)

    return nhs


def build_routes(
    prefixes: List[str], nexthops: List[str]
) -> List[network_types.UnicastRoute]:
    """
    Build list of UnicastRoute using prefixes and nexthops list
    """

    prefixes_str = [ipnetwork.ip_str_to_prefix(p) for p in prefixes]
    nhs = build_nexthops(nexthops)
    return [
        network_types.UnicastRoute(
            dest=p,
            nextHops=[network_types.NextHopThrift(address=nh) for nh in nhs],
        )
        for p in prefixes_str
    ]


def get_route_as_dict_in_str(
    routes: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    route_type: str = "unicast",
) -> Dict[str, str]:
    """
    Convert a routeDb into a dict representing routes in string format
    """

    routes_dict = {}
    # Thrift object instances do not have hash support
    # Make custom stringified object so we can hash and diff
    # dict of prefixes(str) : nexthops(str)
    if route_type == "unicast":
        routes_dict = {
            # pyre-fixme[16]: `MplsRoute` has no attribute `dest`.
            ipnetwork.sprint_prefix(route.dest): sorted(
                ip_nexthop_to_str(nh, True) for nh in route.nextHops
            )
            for route in routes
        }
    elif route_type == "mpls":
        routes_dict = {
            # pyre-fixme[16]: `UnicastRoute` has no attribute `topLabel`.
            str(route.topLabel): sorted(
                ip_nexthop_to_str(nh, True, True) for nh in route.nextHops
            )
            for route in routes
        }
    else:
        assert 0, "Unknown route type %s" % route_type

    return routes_dict


def get_route_as_dict(
    routes: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    route_type: str = "unicast",
) -> Dict[str, Union[network_types.UnicastRoute, network_types.MplsRoute]]:
    """
    Convert a routeDb into a dict representing routes:
    (K, V) = (UnicastRoute.dest/MplsRoute.topLabel, UnicastRoute/MplsRoute)
    """
    routes_dict = {}

    if route_type == "unicast":
        for route in routes:
            # pyre-fixme[16]: `MplsRoute` has no attribute `dest`.
            routes_dict[ipnetwork.sprint_prefix(route.dest)] = route
    elif route_type == "mpls":
        for route in routes:
            # pyre-fixme[16]: `UnicastRoute` has no attribute `topLabel`.
            routes_dict[str(route.topLabel)] = route
    else:
        assert 0, "Unknown route type %s" % route_type

    return routes_dict


def routes_difference(
    lhs: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    rhs: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    route_type: str = "unicast",
) -> List[Union[network_types.UnicastRoute, network_types.MplsRoute]]:
    """
    Get routeDb delta between provided inputs
    """

    diff = []

    # dict of prefixes(str) : nexthops(str)
    _lhs = get_route_as_dict(lhs, route_type)
    _rhs = get_route_as_dict(rhs, route_type)

    # diff_keys will be:
    #   1. dest for network_types.UnicastRoute
    #   2. topLabel for network_types.MplsRoute
    diff_keys = set(_lhs) - set(_rhs)
    for key in diff_keys:
        diff.append(_lhs[key])

    return diff


def prefixes_with_different_nexthops(
    lhs: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    rhs: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    route_type: str,
) -> List[Tuple[str, str, str]]:
    """
    Get keys common to both routeDbs with different nexthops
    """

    keys = []

    # dict of:
    # 1. prefix(str) : nexthops(str) for UnicastRoute
    # 2. topLabel(str) : nexthops(str) for MplsRoute
    _lhs = get_route_as_dict_in_str(lhs, route_type)
    _rhs = get_route_as_dict_in_str(rhs, route_type)
    common_keys = set(_lhs) & set(_rhs)

    if route_type == "unicast":
        for key in common_keys:
            if _lhs[key] != _rhs[key]:
                keys.append((key, _lhs[key], _rhs[key]))
    elif route_type == "mpls":
        for key in common_keys:
            nh_diff_found = False
            l_nh_coll = _lhs[key]
            r_nh_coll = _rhs[key]

            if l_nh_coll == r_nh_coll:
                continue

            # iterate through every single nexthop for this key
            for index in range(0, len(l_nh_coll)):
                l_nh = l_nh_coll[index]
                r_nh = r_nh_coll[index]
                if l_nh != r_nh:
                    l_tokens = l_nh.split()
                    r_tokens = r_nh.split()
                    # Skip verification for MPLS 'POP_AND_LOOKUP' action since
                    # different agent underneath can give different result.
                    if (
                        l_tokens[1] == "POP_AND_LOOKUP"
                        and r_tokens[1] == "POP_AND_LOOKUP"
                    ):
                        continue
                    nh_diff_found = True

            # nexthop diff found
            if nh_diff_found:
                print(key, l_nh_coll, r_nh_coll)
                keys.append((key, l_nh_coll, r_nh_coll))

    return keys


def _only_mpls_routes(
    all_routes: List[Union[network_types.UnicastRoute, network_types.MplsRoute]]
) -> List[network_types.MplsRoute]:
    return [r for r in all_routes if isinstance(r, network_types.MplsRoute)]


def _only_unicast_routes(
    all_routes: List[Union[network_types.UnicastRoute, network_types.MplsRoute]]
) -> List[network_types.UnicastRoute]:
    return [r for r in all_routes if isinstance(r, network_types.UnicastRoute)]


def compare_route_db(
    routes_a: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    routes_b: List[Union[network_types.UnicastRoute, network_types.MplsRoute]],
    route_type: str,
    sources: List[str],
    quiet: bool = False,
) -> Tuple[bool, List[str]]:

    extra_routes_in_a = routes_difference(routes_a, routes_b, route_type)
    extra_routes_in_b = routes_difference(routes_b, routes_a, route_type)
    diff_prefixes = prefixes_with_different_nexthops(routes_a, routes_b, route_type)

    # return error type
    error_msg = []

    # if all good, then return early
    if not extra_routes_in_a and not extra_routes_in_b and not diff_prefixes:
        if not quiet:
            if is_color_output_supported():
                click.echo(click.style("PASS", bg="green", fg="black"))
            else:
                click.echo("PASS")
            print("{} and {} routing table match".format(*sources))
        return True, error_msg

    # Something failed.. report it
    if not quiet:
        if is_color_output_supported():
            click.echo(click.style("FAIL", bg="red", fg="black"))
        else:
            click.echo("FAIL")
        print("{} and {} routing table do not match".format(*sources))
    if extra_routes_in_a:
        caption = "Routes in {} but not in {}".format(*sources)
        if not quiet:
            if route_type == "unicast":
                print_unicast_routes(caption, _only_unicast_routes(extra_routes_in_a))
            elif route_type == "mpls":
                print_mpls_routes(caption, _only_mpls_routes(extra_routes_in_a))
        else:
            error_msg.append(caption)

    if extra_routes_in_b:
        caption = "Routes in {} but not in {}".format(*reversed(sources))
        if not quiet:
            if route_type == "unicast":
                print_unicast_routes(caption, _only_unicast_routes(extra_routes_in_b))
            elif route_type == "mpls":
                print_mpls_routes(caption, _only_mpls_routes(extra_routes_in_b))
        else:
            error_msg.append(caption)

    if diff_prefixes:
        caption = "Prefixes have different nexthops in {} and {}".format(*sources)
        rows = []
        for prefix, lhs_nexthops, rhs_nexthops in diff_prefixes:
            rows.append([prefix, len(lhs_nexthops), len(rhs_nexthops)])
        column_labels = ["Prefix"] + sources
        if not quiet:
            print(
                printing.render_horizontal_table(rows, column_labels, caption=caption)
            )
        else:
            error_msg.append(caption)
    return False, error_msg


def validate_route_nexthops(routes, interfaces, sources, quiet=False):
    """
    Validate between fib routes and lm interfaces

    :param routes: list network_types.UnicastRoute (structured routes)
    :param interfaces: dict<interface-name, InterfaceDetail>
    """

    # record invalid routes in dict<error, list<route_db>>
    invalid_routes = defaultdict(list)

    # define error types
    MISSING_NEXTHOP = "Nexthop does not exist"
    INVALID_SUBNET = "Nexthop address is not in the same subnet as interface"
    INVALID_LINK_LOCAL = "Nexthop address is not link local"

    # return error type
    error_msg = []

    for route in routes:
        dest = ipnetwork.sprint_prefix(route.dest)
        # record invalid nexthops in dict<error, list<nexthops>>
        invalid_nexthop = defaultdict(list)
        for nextHop in route.nextHops:
            nh = nextHop.address

            # if nexthop addr is v6 link-local, then ifName must be specified
            if (
                ipnetwork.ip_version(nh.addr) == 6
                and ipnetwork.is_link_local(nh.addr)
                and not nh.ifName
            ):
                invalid_nexthop[INVALID_LINK_LOCAL].append(nextHop)

            # next-hop can be empty for other types. Skip if it is the case
            if nh.ifName is None:
                continue

            if nh.ifName not in interfaces or not interfaces[nh.ifName].info.isUp:
                invalid_nexthop[MISSING_NEXTHOP].append(nextHop)
                continue
            # if nexthop addr is v4, make sure it belongs to same subnets as
            # interface addr
            if ipnetwork.ip_version(nh.addr) == 4:
                networks = interfaces[nh.ifName].info.networks
                if networks is None:
                    # maintain backward compatbility
                    networks = []
                for prefix in networks:
                    if ipnetwork.ip_version(
                        prefix.prefixAddress.addr
                    ) == 4 and not ipnetwork.is_same_subnet(
                        nh.addr, prefix.prefixAddress.addr, "31"
                    ):
                        invalid_nexthop[INVALID_SUBNET].append(nextHop)

        # build routes per error type
        for k, v in invalid_nexthop.items():
            invalid_routes[k].append(
                network_types.UnicastRoute(dest=route.dest, nextHops=v)
            )

    # if all good, then return early
    if not invalid_routes:
        if not quiet:
            if is_color_output_supported():
                click.echo(click.style("PASS", bg="green", fg="black"))
            else:
                click.echo("PASS")
            print("Route validation successful")
        return True, error_msg

    # Something failed.. report it
    if not quiet:
        if is_color_output_supported():
            click.echo(click.style("FAIL", bg="red", fg="black"))
        else:
            click.echo("FAIL")
        print("Route validation failed")
    # Output report per error type
    for err, route_db in invalid_routes.items():
        caption = "Error: {}".format(err)
        if not quiet:
            print_unicast_routes(caption, route_db)
        else:
            error_msg.append(caption)

    return False, error_msg


def mpls_action_to_str(
    mpls_action: Union[network_types.MplsAction, network_types_py3.MplsAction]
) -> str:
    """
    Convert Network.MplsAction to string representation
    """

    if isinstance(mpls_action, network_types.MplsAction):
        action_str = network_types.MplsActionCode._VALUES_TO_NAMES.get(
            mpls_action.action, ""
        )
    else:
        action_str = mpls_action.action.name

    label_str = ""
    if mpls_action.swapLabel is not None:
        label_str = f" {mpls_action.swapLabel}"
    push_labels = mpls_action.pushLabels
    if push_labels is not None:
        label_str = f" {'/'.join(str(l) for l in push_labels)}"
    return f"mpls {action_str}{label_str}"


def ip_nexthop_to_str(
    nextHop: Union[network_types.NextHopThrift, network_types_py3.NextHopThrift],
    ignore_v4_iface: bool = False,
    ignore_v6_iface: bool = False,
) -> str:
    """
    Convert Network.BinaryAddress to string representation of a nexthop
    """

    nh = nextHop.address
    ifName = "%{}".format(nh.ifName) if nh.ifName else ""
    if len(nh.addr) == 4 and ignore_v4_iface:
        ifName = ""
    if len(nh.addr) == 16 and ignore_v6_iface:
        ifName = ""
    addr_str = "{}{}".format(ipnetwork.sprint_addr(nh.addr), ifName)

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


def print_unicast_routes(
    caption: str,
    unicast_routes: Union[
        Sequence[network_types_py3.UnicastRoute], List[network_types.UnicastRoute]
    ],
    prefixes: Optional[List[str]] = None,
    element_prefix: str = ">",
    element_suffix: str = "",
    filter_exact_match: bool = False,
    timestamp: bool = False,
) -> None:
    """
    Print unicast routes. Subset specified by prefixes will be printed if specified
        :param caption: Title of the table (a string).
        :param unicast_routes: Unicast routes
        :param prefixes: Optional prefixes/filter to print.
        :param element_prefix: Starting prefix for each item. (string)
        :param element_suffix: Ending/terminator for each item. (string)
        :param filter_exact_match: Indicate exact match or subnet match.
        :param timestamp: Prints time for each item. (bool)
    """

    networks = None
    if prefixes:
        networks = [ipaddress.ip_network(p) for p in prefixes]

    route_strs = []
    for route in unicast_routes:
        entry = build_unicast_route(route, filter_for_networks=networks)
        if entry:
            dest, nexthops = entry
            paths_str = "\n".join(["  via {}".format(nh) for nh in nexthops])
            route_strs.append([dest, paths_str])

    print(
        printing.render_vertical_table(
            route_strs,
            caption=caption,
            element_prefix=element_prefix,
            element_suffix=element_suffix,
            timestamp=timestamp,
        )
    )


def build_unicast_route(
    route: Union[network_types_py3.UnicastRoute, network_types.UnicastRoute],
    filter_for_networks: Optional[
        List[Union[ipaddress.IPv4Network, ipaddress.IPv6Network]]
    ] = None,
    filter_exact_match: bool = False,
) -> Tuple[str, List[str]]:
    """
    Build unicast route.
        :param route: Unicast Route
        :param filter_for_networks: IP/Prefixes to filter.
        :param filter_exact_match: Indicate exact match or subnet match.
    """
    dest = ipnetwork.sprint_prefix(route.dest)
    if filter_for_networks:
        if filter_exact_match:
            if not ipaddress.ip_network(dest) in filter_for_networks:
                return ("", [])
        else:
            if not ipnetwork.contain_any_prefix(dest, filter_for_networks):
                return ("", [])
    nexthops = [ip_nexthop_to_str(nh) for nh in route.nextHops]
    return dest, nexthops


def print_mpls_routes(
    caption: str,
    mpls_routes: Union[
        List[network_types.MplsRoute], Sequence[network_types_py3.MplsRoute]
    ],
    labels: Optional[List[int]] = None,
    element_prefix: str = ">",
    element_suffix: str = "",
    timestamp: bool = False,
) -> None:
    """
    List mpls routes. Subset specified by labels will be printed if specified
        :param caption: Title of the table (a string).
        :param mpls_routes: mpls routes
        :param labels: Optional labels/filter to print.
        :param element_prefix: Starting prefix for each item. (string)
        :param element_suffix: Ending/terminator for each item. (string)
        :param timestamp: Prints time for each item. (bool)
    """

    route_strs = []
    for route in mpls_routes:
        if labels and route.topLabel not in labels:
            continue

        paths_str = "\n".join(
            ["  via {}".format(ip_nexthop_to_str(nh)) for nh in route.nextHops]
        )
        route_strs.append([str(route.topLabel), paths_str])

    print(
        printing.render_vertical_table(
            route_strs,
            caption=caption,
            element_prefix=element_prefix,
            element_suffix=element_suffix,
            timestamp=timestamp,
        )
    )


def get_routes_json(
    host: str,
    client: int,
    routes: List[network_types.UnicastRoute],
    prefixes: Optional[List[str]],
    mpls_routes: Optional[List[network_types.MplsRoute]],
    labels: Optional[List[int]],
):
    networks = None
    if prefixes:
        networks = [ipaddress.ip_network(p) for p in prefixes]

    data = {"host": host, "client": client, "routes": [], "mplsRoutes": []}

    for route in routes:
        dest = ipnetwork.sprint_prefix(route.dest)
        if not ipnetwork.contain_any_prefix(dest, networks):
            continue
        route_data = {
            "dest": dest,
            "nexthops": [ip_nexthop_to_str(nh) for nh in route.nextHops],
        }
        # pyre-fixme[16]: `int` has no attribute `append`.
        data["routes"].append(route_data)

    if mpls_routes:
        for label in mpls_routes:
            dest = label.topLabel
            if labels and dest not in labels:
                continue
            route_data = {
                "dest": dest,
                "nexthops": [ip_nexthop_to_str(nh) for nh in label.nextHops],
            }
            data["mplsRoutes"].append(route_data)

    return data


def get_routes(
    route_db: openr_types.RouteDatabase,
) -> Tuple[List[network_types.UnicastRoute], List[network_types.MplsRoute]]:
    """
    Find all routes for each prefix in routeDb

    :param route_db: RouteDatabase
    :return (
        list of UnicastRoute of prefix & corresponding shortest nexthops
        list of MplsRoute of prefix & corresponding shortest nexthops
    )
    """

    unicast_routes, mpls_routes = None, None
    unicast_routes = sorted(
        route_db.unicastRoutes, key=lambda x: x.dest.prefixAddress.addr
    )
    mpls_routes = sorted(route_db.mplsRoutes, key=lambda x: x.topLabel)

    return (unicast_routes, mpls_routes)


def print_spt_infos(
    spt_infos: openr_types.SptInfos,
    roots: List[str],
    area: Optional[str] = None,
) -> None:
    """
    print spanning tree information
    """

    output = []

    area = f'Area "{area}": ' if area is not None else ""
    # step0. print current flooding root id
    print(f"\n>{area}Current Flood On Root-Id: {spt_infos.floodRootId}")

    # step1. print neighbor level counters
    caption = "Neighbor DUAL Counters"
    column_labels = ["Neighbor", "Pkt(Tx/Rx)", "Msg(Tx/Rx)", "Is Flooding Peer"]
    neighbor_counters = spt_infos.counters.neighborCounters
    rows = []
    for nb, counters in neighbor_counters.items():
        state = click.style("No")
        if nb in spt_infos.floodPeers:
            state = click.style("Yes", fg="green")
        rows.append(
            [
                nb,
                "{}/{}".format(counters.pktSent, counters.pktRecv),
                "{}/{}".format(counters.msgSent, counters.msgRecv),
                state,
            ]
        )
    seg = printing.render_horizontal_table(rows, column_labels, tablefmt="plain")
    output.append([caption, seg])

    # step2. print root level counters
    root_counters = spt_infos.counters.rootCounters
    column_labels = [
        "Neighbor",
        "Query(Tx/Rx)",
        "Reply(Tx/Rx)",
        "Update(Tx/Rx)",
        "Total(Tx/Rx)",
        "Role",
    ]
    for root, info in spt_infos.infos.items():
        if roots is not None and root not in roots:
            continue
        if info.passive:
            state = click.style("PASSIVE", fg="green")
        else:
            state = click.style("ACTIVE", fg="red")
        cap = "root@{}[{}]: parent: {}, cost: {}, ({}) children".format(
            root, state, info.parent, info.cost, len(info.children)
        )
        rows = []
        # pyre-fixme[16]: `Optional` has no attribute `items`.
        for nb, counters in root_counters.get(root).items():
            role = "-"
            if nb == info.parent:
                role = "Parent"
            elif nb in info.children:
                role = "Child"
            if counters.querySent > counters.replyRecv:
                # active-state: I'm expecting receving reply from this neighbor
                # show it as red
                nb = click.style(nb, fg="red")
            rows.append(
                [
                    nb,
                    "{}/{}".format(counters.querySent, counters.queryRecv),
                    "{}/{}".format(counters.replySent, counters.replyRecv),
                    "{}/{}".format(counters.updateSent, counters.updateRecv),
                    "{}/{}".format(counters.totalSent, counters.totalRecv),
                    role,
                ]
            )
        seg = printing.render_horizontal_table(rows, column_labels, tablefmt="plain")
        output.append([cap, seg])
    print(printing.render_vertical_table(output))


def get_areas_list(client: OpenrCtrl.Client) -> Set[str]:
    return {a.area_id for a in client.getRunningConfigThrift().areas}


# This API is used by commands that need one and only one
# area ID. For older images that don't support area feature, this API will
# return 'None'. If area ID is passed, API checks if it's valid and returns
# the same ID
def get_area_id(client: OpenrCtrl.Client, area: str) -> str:
    # if no area is provided, return area in case only one area is configured
    areas = get_areas_list(client)
    if (area is None or area == "") and 1 == len(areas):
        (area,) = areas
        return area

    if area not in areas:
        print(f"Error: Must specify one of the areas: {areas}")
        sys.exit(1)
    return area


@lru_cache(maxsize=1)
def is_color_output_supported() -> bool:
    """
    Check if stdout is a terminal and supports colors
    """
    is_a_tty = hasattr(sys.stdout, "isatty") and sys.stdout.isatty()
    has_color = False

    try:
        # initialize the terminal to get the Terminfo
        curses.setupterm()
        # get the 'colors' info: maximum number of colors on screen
        if curses.tigetnum("colors") >= 8:
            has_color = True
    except Exception:
        pass

    return is_a_tty and has_color


def print_route_details(
    routes: List[
        Union[ctrl_types.AdvertisedRouteDetail, ctrl_types.ReceivedRouteDetail]
    ],
    key_to_str_fn: Callable[[PrintAdvertisedTypes], Tuple[str]],
    detailed: bool,
) -> None:
    """
    Print advertised or received route.

    `key_to_str_fn` argument specifies the transformation of key attributes to tuple of
    strings

    Output format
      > 10.0.0.0/8, entries=10, best-entries(*)=1, best-entry(@)
        [@*] source <key>
             forwarding algo=<fwd-algo> type=<fwd-type>
             metrics=<metrics>
             requirements=<min-nexthops> <prepend-label> <tags?> <area-stack?>


    """

    rows = []
    # print header
    print_route_header(rows, detailed)

    for route_detail in routes:
        best_key = key_to_str_fn(route_detail.bestKey)
        best_keys = {key_to_str_fn(k) for k in route_detail.bestKeys}

        # Create a title for the route
        rows.append(
            f"> {ipnetwork.sprint_prefix(route_detail.prefix)}"
            f", {len(best_keys)}/{len(route_detail.routes)}"
        )

        # Add all entries associated with routes
        for route in route_detail.routes:
            markers = f"{'*' if key_to_str_fn(route.key) in best_keys else ''}{'@' if key_to_str_fn(route.key) == best_key else ' '}"
            print_route_helper(rows, route, key_to_str_fn, detailed, markers)
        rows.append("")

    print("\n".join(rows))


def print_advertised_routes(
    routes: List[ctrl_types.AdvertisedRoute],
    key_to_str_fn: Callable[[PrintAdvertisedTypes], Tuple[str]],
    detailed: bool,
) -> None:
    """
    Print postfilter advertised or rejected route.

    `key_to_str_fn` argument specifies the transformation of key attributes to tuple of
    strings

    Output format
      > 10.0.0.0/8, entries=10, best-entries(*)=1, best-entry(@)
        [@*] source <key>
             forwarding algo=<fwd-algo> type=<fwd-type>
             metrics=<metrics>
             requirements=<min-nexthops> <prepend-label> <tags?> <area-stack?>


    """

    rows = []
    # print header
    print_route_header(rows, detailed)

    for route in routes:
        # Create a title for the route
        rows.append(f"> {ipnetwork.sprint_prefix(route.route.prefix)}")
        print_route_helper(rows, route, key_to_str_fn, detailed, "{*@}")
        rows.append("")

    print("\n".join(rows))


def print_route_header(rows: List[str], detailed: bool):
    """
    Helper function to construct print lines of header for advertised and received rooutes.
    """
    # Add marker information
    rows.append("Markers: * - One of the best entries, @ - The best entry")
    if not detailed:
        rows.append(
            "Acronyms: SP - Source Preference, PP - Path Preference, D - Distance\n"
            "          MN - Min-Nexthops, PL - Prepend Label"
        )
    rows.append("")

    # Add a header if not detailed
    # NOTE: Using very custom formatting here instead of using table rendering
    # for compact output. And also interleave the custom route titles in between
    # This header is in sync with the rows we append
    if not detailed:
        rows.append(
            f"{'':<2} {'Source':<36} "
            f"{'FwdAlgo':<12} {'FwdType':<8} "
            f"{'SP':<6} "
            f"{'PP':<6} "
            f"{'D':<6} "
            f"{'MN':<6}"
            f"{'PL':<6}"
        )
        rows.append("")


def print_route_helper(
    rows: List[str],
    route: Union[ctrl_types.AdvertisedRoute, ctrl_types.ReceivedRoute],
    key_to_str_fn: Callable[[PrintAdvertisedTypes], Tuple[str]],
    detailed: bool,
    markers: str,
) -> None:
    """
    Construct print lines of advertised route, append to rows

    `key_to_str_fn` argument specifies the transformation of key attributes to tuple of
    strings
    `markers` argument specifies [@*] to indicate route is ecmp/best entry

    Output format
        [@*] source <key>
             forwarding algo=<fwd-algo> type=<fwd-type>
             metrics=<metrics>
             requirements=<min-nexthops> <prepend-label> <tags?> <area-stack?>


    """

    key, metrics = key_to_str_fn(route.key), route.route.metrics
    fwd_algo = config_types.PrefixForwardingAlgorithm._VALUES_TO_NAMES.get(
        route.route.forwardingAlgorithm
    )
    fwd_type = config_types.PrefixForwardingType._VALUES_TO_NAMES.get(
        route.route.forwardingType
    )
    if detailed:
        rows.append(f"{markers} from {' '.join(key)}")
        rows.append(f"     Forwarding - algorithm: {fwd_algo}, type: {fwd_type}")
        rows.append(
            f"     Metrics - path-preference: {metrics.path_preference}"
            f", source-preference: {metrics.source_preference}"
            f", distance: {metrics.distance}"
        )
        if route.route.minNexthop:
            rows.append(f"     Performance - min-nexthops: {route.route.minNexthop}")
        if route.route.prependLabel:
            rows.append(f"     Misc - prepend-label: {route.route.prependLabel}")
        rows.append(f"     Tags - {', '.join(route.route.tags)}")
        rows.append(f"     Area Stack - {', '.join(route.route.area_stack)}")
        if (
            isinstance(route, ctrl_types.AdvertisedRoute)
            and hasattr(route, "hitPolicy")
            and route.hitPolicy
        ):
            rows.append(f"     Policy - {route.hitPolicy}")
    else:
        min_nexthop = (
            route.route.minNexthop if route.route.minNexthop is not None else "-"
        )
        prepend_label = (
            route.route.prependLabel if route.route.prependLabel is not None else "-"
        )
        rows.append(
            f"{markers:<2} {' '.join(key)[:36]:<36} "
            f"{fwd_algo:<12} {fwd_type:<8} "
            f"{metrics.source_preference:<6} "
            f"{metrics.path_preference:<6} "
            f"{metrics.distance:<6} "
            f"{min_nexthop:<6}"
            f"{prepend_label:<6}"
        )
