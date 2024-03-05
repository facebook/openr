#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import copy
import curses
import datetime
import ipaddress
import json
import re
import sys
from builtins import input, map
from collections import defaultdict
from functools import lru_cache, partial
from io import TextIOBase
from itertools import product
from typing import (
    Any,
    Callable,
    Dict,
    Iterable,
    List,
    Mapping,
    Optional,
    Sequence,
    Set,
    Tuple,
    Union,
)

import bunch
import click
from openr.clients.openr_client import get_openr_ctrl_client_py
from openr.KvStore import ttypes as kv_store_types_py
from openr.Network import ttypes as network_types_py
from openr.OpenrCtrl import ttypes as ctrl_types_py
from openr.thrift.KvStore import thrift_types as kv_store_types
from openr.thrift.Network import thrift_types as network_types
from openr.thrift.OpenrCtrl import thrift_types as ctrl_types
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Types import thrift_types as openr_types
from openr.Types import ttypes as openr_types_py
from openr.utils import ipnetwork, printing
from openr.utils.consts import Consts
from openr.utils.serializer import deserialize_thrift_py_object, object_to_dict
from thrift.python.serializer import deserialize


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


def yesno(question, skip_confirm: bool = False) -> bool:
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


def json_dumps(data) -> str:
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


def time_since(timestamp) -> str:
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


def parse_nodes(cli_opts: bunch.Bunch, nodes: str) -> Set[str]:
    """parse nodes from user input

    :return set: the set of nodes
    """

    if not nodes:
        with get_openr_ctrl_client_py(cli_opts.host, cli_opts) as client:
            nodes = client.getMyNodeName()
    nodes_set = set(nodes.strip().split(","))

    return nodes_set


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
                ipnetwork.sprint_prefix(prefix_entry.prefix),
                ipnetwork.sprint_prefix_type(prefix_entry.type),
                ipnetwork.sprint_prefix_forwarding_type(prefix_entry.forwardingType),
                ipnetwork.sprint_prefix_forwarding_algorithm(
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


def parse_prefix_database(
    prefix_filter: Optional[Union[network_types.IpPrefix, str]],
    client_type_filter: Optional[Union[network_types.PrefixType, str]],
    prefix_dbs: Dict[str, openr_types.PrefixDatabase],
    prefix_db: Any,
) -> None:
    """
    Utility function to prase `prefix_db` with filter and populate prefix_dbs
    accordingly
    """
    if client_type_filter and isinstance(client_type_filter, str):
        PREFIX_TYPE_TO_VALUES = {e.name: e for e in network_types.PrefixType}
        client_type_filter = PREFIX_TYPE_TO_VALUES.get(client_type_filter.upper(), None)
        if client_type_filter is None:
            raise Exception(
                f"Unknown client type. Use one of {list(PREFIX_TYPE_TO_VALUES.keys())}"
            )

    if prefix_filter:
        if isinstance(prefix_filter, str):
            prefix_filter = ipnetwork.ip_str_to_prefix(prefix_filter)

    if isinstance(prefix_db, kv_store_types.Value):
        if prefix_db.value:
            prefix_db = deserialize(openr_types.PrefixDatabase, prefix_db.value)
        else:
            prefix_db = openr_types.PrefixDatabase()

    if prefix_db.deletePrefix:
        # In per prefix-key, deletePrefix flag is set to indicate prefix
        # withdrawl
        return

    if prefix_db.thisNodeName not in prefix_dbs:
        prefix_dbs[prefix_db.thisNodeName] = openr_types.PrefixDatabase(
            thisNodeName=f"{prefix_db.thisNodeName}", prefixEntries=[]
        )

    # update prefixEntries, we cannot modify thrift-python attributes and hence we need to create prefix_entries and replace prefix_dbs[prefix_db.thisNodeName]
    prefix_entries = list(prefix_dbs[prefix_db.thisNodeName].prefixEntries)
    for prefix_entry in prefix_db.prefixEntries:
        if prefix_filter and prefix_filter != prefix_entry.prefix:
            continue
        if client_type_filter and client_type_filter != prefix_entry.type:
            continue
        prefix_entries.append(prefix_entry)
    prefix_dbs[prefix_db.thisNodeName] = prefix_dbs[prefix_db.thisNodeName](
        prefixEntries=prefix_entries
    )


def print_prefixes_table(resp, nodes, prefix, client_type, iter_func) -> None:
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
    """convert thrift-py3/python instance into a dict in strings

    :param thrift_inst: a thrift-python/thrift-py3 instance
    :param update_func: transformation function to update dict value of
                        thrift object. It is optional.

    :return dict: dict with attributes as key, value in strings
    """

    if thrift_inst is None:
        return None

    # We cannot copy thrift_inst.__dict__ in thrift-py3/python
    gen_dict = {}
    for field_name, field_value in thrift_inst:
        gen_dict[field_name] = field_value

    if update_func is not None:
        update_func(gen_dict, thrift_inst)

    return gen_dict


# to be deprecated
def thrift_py_to_dict(thrift_inst, update_func=None):
    """convert thrift-py instance into a dict in strings

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


def collate_prefix_keys(
    kvstore_keyvals: Mapping[str, kv_store_types.Value],
) -> Dict[str, openr_types.PrefixDatabase]:
    """collate all the prefixes of node and return a map of
    nodename - PrefixDatabase in thrift-python
    """

    prefix_maps = {}
    for key, value in sorted(kvstore_keyvals.items()):
        if key.startswith(Consts.PREFIX_DB_MARKER):

            node_name = key.split(":")[1]
            if value.value:
                prefix_db = deserialize(openr_types.PrefixDatabase, value.value)
            else:
                prefix_db = openr_types.PrefixDatabase()

            if prefix_db.deletePrefix:
                continue

            if node_name not in prefix_maps:
                prefix_maps[node_name] = openr_types.PrefixDatabase(
                    thisNodeName=f"{node_name}", prefixEntries=[]
                )

            # include prefix_db.prefixEntries in prefix_maps[node_name].prefixEntries
            # notice that we cannot modify attributes of a thrift-python struct but replace the whole struct
            # so we need to create prefix_entries and replace prefix_maps[node_name]
            prefix_entries = list(prefix_maps[node_name].prefixEntries)
            for prefix_entry in prefix_db.prefixEntries:
                prefix_entries.append(prefix_entry)
            prefix_maps[node_name] = prefix_maps[node_name](
                prefixEntries=prefix_entries
            )

    return prefix_maps


def prefix_entry_to_dict(prefix_entry):
    """convert prefixEntry from thrift-py3/thrift-python instance into a dict in strings"""

    def _update(prefix_entry_dict, prefix_entry):
        # prefix and data need string conversion and metric_vector can be
        # represented as a dict so we update them
        prefix_entry_dict.update(
            {
                "prefix": ipnetwork.sprint_prefix(prefix_entry.prefix),
                "metrics": thrift_to_dict(prefix_entry.metrics),
                "tags": list(prefix_entry.tags if prefix_entry.tags else []),
                "area_stack": list(
                    prefix_entry.area_stack if prefix_entry.area_stack else []
                ),
            }
        )

    return thrift_to_dict(prefix_entry, _update)


def prefix_db_to_dict(prefix_db: Any) -> Dict[str, Any]:
    """convert PrefixDatabase from thrift-py3/thrift-python instance to a dictionary"""

    if isinstance(prefix_db, kv_store_types.Value):
        if prefix_db.value:
            prefix_db = deserialize(openr_types.PrefixDatabase, prefix_db.value)
        else:
            prefix_db = openr_types.PrefixDatabase()

    def _update(prefix_db_dict, prefix_db):
        prefix_db_dict.update(
            {"prefixEntries": list(map(prefix_entry_to_dict, prefix_db.prefixEntries))}
        )

    return thrift_to_dict(prefix_db, _update)


def print_prefixes_json(resp, nodes, prefix, client_type, iter_func) -> None:
    """print prefixes in json"""

    prefixes_map = {}
    iter_func(
        prefixes_map, resp, nodes, partial(parse_prefix_database, prefix, client_type)
    )
    for node_name, prefix_db in prefixes_map.items():
        prefixes_map[node_name] = prefix_db_to_dict(prefix_db)
    print(json_dumps(prefixes_map))


def update_global_adj_db(global_adj_db, adj_db) -> None:
    """update the global adj map based on publication from single node

    :param global_adj_map map(node, AdjacencyDatabase)
        the map for all adjacencies in the network - to be updated
    :param adj_db openr_types_py.AdjacencyDatabase: publication from single
        node
    """

    assert isinstance(adj_db, openr_types_py.AdjacencyDatabase)

    global_adj_db[adj_db.thisNodeName] = adj_db


def build_global_adj_db(resp):
    """build a map of all adjacencies in the network. this is used
    for bi-directional validation

    :param resp kv_store_types_py.Publication: the parsed publication

    :return map(node, AdjacencyDatabase): the global
        adj map, devices name mapped to devices it connects to, and
        properties of that connection
    """

    # map: (node) -> AdjacencyDatabase)
    global_adj_db = {}

    for key, value in resp.keyVals.items():
        if not key.startswith(Consts.ADJ_DB_MARKER):
            continue
        adj_db = deserialize_thrift_py_object(
            value.value, openr_types_py.AdjacencyDatabase
        )
        update_global_adj_db(global_adj_db, adj_db)

    return global_adj_db


def build_global_prefix_db(resp):
    """build a map of all prefixes in the network. this is used
    for checking for changes in topology

    :param resp kv_store_types_py.Publication: the parsed publication

    :return map(node, set([prefix])): the global prefix map,
        prefixes mapped to the node
    """

    # map: (node) -> set([prefix])
    global_prefix_db = {}
    prefix_maps = collate_prefix_keys(resp.keyVals)

    for _, prefix_db in prefix_maps.items():
        update_global_prefix_db(global_prefix_db, prefix_db)

    return global_prefix_db


def dump_adj_db_full(global_adj_db, adj_db, bidir) -> Tuple[
    int,
    bool,
    Union[List[openr_types_py.Adjacency], Sequence[openr_types.Adjacency]],
    str,
]:
    """given an adjacency database, dump neighbors. Use the
        global adj database to validate bi-dir adjacencies

    :param global_adj_db map(str, AdjacencyDatabase):
        map of node names to their adjacent node names
    :param adj_db openr_types_py.AdjacencyDatabase: latest from kv store
    :param bidir bool: only dump bidir adjacencies

    :return (<param1>, <param2>, ...,  [adjacencies]): tuple of params and
     list of adjacencies
    """

    assert isinstance(adj_db, openr_types.AdjacencyDatabase) or isinstance(
        adj_db, openr_types_py.AdjacencyDatabase
    )
    this_node_name = adj_db.thisNodeName
    area = adj_db.area if adj_db.area is not None else "N/A"
    node_metric_increment_val = (
        adj_db.nodeMetricIncrementVal
        if adj_db.nodeMetricIncrementVal is not None
        else 0
    )

    if not bidir:
        return (
            node_metric_increment_val,
            adj_db.isOverloaded,
            adj_db.adjacencies,
            area,
        )

    # filter out non bi-dir adjacencies
    adjacencies = []

    for adj in adj_db.adjacencies:
        other_node_db = global_adj_db.get(adj.otherNodeName, None)
        if other_node_db is None:
            continue
        # attention: store peer's adjacency overload status for a potential
        # overriding with better displaying purpose
        other_node_neighbors = {
            (adjacency.otherNodeName, adjacency.otherIfName): adjacency.isOverloaded
            for adjacency in other_node_db.adjacencies
        }
        # skip the uni-directional adjacencies since we only display bi-dir
        if (this_node_name, adj.ifName) not in other_node_neighbors.keys():
            continue
        # if one side of adjacency is OVERLOADED. Set the other side as well.
        adj_updated = None
        if isinstance(adj, openr_types.Adjacency):
            adj_updated = openr_types.Adjacency(
                otherNodeName=adj.otherNodeName,
                ifName=adj.ifName,
                nextHopV6=adj.nextHopV6,
                nextHopV4=adj.nextHopV4,
                metric=adj.metric,
                adjLabel=adj.adjLabel,
                isOverloaded=adj.isOverloaded
                | other_node_neighbors[(this_node_name, adj.ifName)],
                rtt=adj.rtt,
                timestamp=adj.timestamp,
                weight=adj.weight,
                otherIfName=adj.otherIfName,
                adjOnlyUsedByOtherNode=adj.adjOnlyUsedByOtherNode,
            )
        else:
            adj_updated = openr_types_py.Adjacency(
                otherNodeName=adj.otherNodeName,
                ifName=adj.ifName,
                nextHopV6=adj.nextHopV6,
                nextHopV4=adj.nextHopV4,
                metric=adj.metric,
                adjLabel=adj.adjLabel,
                isOverloaded=adj.isOverloaded
                | other_node_neighbors[(this_node_name, adj.ifName)],
                rtt=adj.rtt,
                timestamp=adj.timestamp,
                weight=adj.weight,
                otherIfName=adj.otherIfName,
                adjOnlyUsedByOtherNode=adj.adjOnlyUsedByOtherNode,
            )
        adjacencies.append(adj_updated)

    return (
        adj_db.nodeMetricIncrementVal,
        adj_db.isOverloaded,
        adjacencies,
        area,
    )


def adj_db_to_dict(adjs_map, adj_dbs, adj_db, bidir, version) -> None:
    """convert adj db to dict"""

    (
        node_metric_increment_val,
        is_overloaded,
        adjacencies,
        area,
    ) = dump_adj_db_full(adj_dbs, adj_db, bidir)

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

        if isinstance(adj, openr_types.Adjacency):
            return thrift_to_dict(adj, _update)
        else:
            return thrift_py_to_dict(adj, _update)

    adjacencies = list(map(adj_to_dict, adjacencies))

    # Dump is keyed by node name with attrs as key values
    adjs_map[adj_db.thisNodeName] = {
        "is_overloaded": is_overloaded,
        "adjacencies": adjacencies,
        "area": area,
        "node_metric_increment_val": node_metric_increment_val,
    }


def adj_dbs_to_dict(resp, nodes, bidir, iter_func):
    """get parsed adjacency db

    :param resp kv_store_types_py.Publication, or decision_types.adjDbs
    :param nodes set: the set of the nodes to print prefixes for
    :param bidir bool: only dump bidirectional adjacencies

    :return map(node, map(adjacency_keys, (adjacency_values)): the parsed
        adjacency DB in a map with keys and values in strings
    """

    adj_dbs = resp
    if isinstance(adj_dbs, kv_store_types_py.Publication):
        adj_dbs = build_global_adj_db(resp)

    def _parse_adj(adjs_map, adj_db):
        version = None
        if isinstance(adj_db, kv_store_types_py.Value):
            version = adj_db.version
            adj_db = deserialize_thrift_py_object(
                adj_db.value, openr_types_py.AdjacencyDatabase
            )
        if isinstance(adj_db, kv_store_types.Value):
            version = adj_db.version
            if adj_db.value:
                adj_db = deserialize(openr_types.AdjacencyDatabase, adj_db.value)
            else:
                adj_db = openr_types.AdjacencyDatabase()

        adj_db_to_dict(adjs_map, adj_dbs, adj_db, bidir, version)

    adjs_map = {}
    iter_func(adjs_map, resp, nodes, _parse_adj)
    return adjs_map


def adj_dbs_to_area_dict(
    resp: Union[
        List[openr_types_py.AdjacencyDatabase], Sequence[openr_types.AdjacencyDatabase]
    ],
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


def print_json(map: Dict, file: Optional[TextIOBase] = None) -> None:
    """
    Print object in json format. Use this function for consistent json style
    formatting for output. Further it prints to `stdout` stream.
    - file: Defaults to none which is sys.stdout - None causes a runtime
            evauluation to find the current sys.stdout rather than a import
            time evaluation. Handy for unittests where stdout is mocked.

    @map: object that needs to be printed in json
    """

    print(json_dumps(map), file=file)


def print_adjs_table(adjs_map, neigh=None, interface=None) -> None:
    """print adjacencies

    :param adjacencies as list of dict
    """

    column_labels = [
        "Neighbor",
        "Local Intf",
        "Remote Intf",
        "Metric",
        "NextHop-v4",
        "NextHop-v6",
        "Uptime",
        "Area",
    ]

    output = []
    adj_found = False
    for node, val in sorted(adjs_map.items()):
        adj_tokens = []

        # [Hard-Drain] report overloaded if it is overloaded
        is_overloaded = val["is_overloaded"]
        if is_overloaded:
            overload_str = f"{is_overloaded}"
            if is_color_output_supported():
                overload_str = click.style(overload_str, fg="red")
            adj_tokens.append(f"Overloaded: {overload_str}")

        # [Soft-Drain] report node metric increment if it is soft-drained
        node_metric_inc = val["node_metric_increment_val"]
        if int(node_metric_inc) > 0:
            node_metric_inc_str = f"{node_metric_inc}"
            if is_color_output_supported():
                node_metric_inc_str = click.style(node_metric_inc_str, fg="red")
            adj_tokens.append(f"Node Metric Increment: {node_metric_inc_str}")

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
    title_tokens.append("Overloaded: {}".format(overload_str))

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


def next_hop_thrift_to_dict(nextHop: network_types.NextHopThrift) -> Dict[str, Any]:
    """convert nextHop from thrift-py3/thrift-python instance into a dict in strings"""
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
    """convert route from thrift-py3/thrift-python instance into a dict in strings"""

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
    Convert MPLS route (thrift-py3/thrift-python) to json serializable dict object
    """

    def _update(route_dict, route: network_types.MplsRoute):
        route_dict.update(
            {"nextHops": [next_hop_thrift_to_dict(nh) for nh in route.nextHops]}
        )

    return thrift_to_dict(route, _update)


def route_db_to_dict(route_db: openr_types.RouteDatabase) -> Dict[str, Any]:
    """
    Convert route from thrift-py3/thrift-python instance into a dict in strings
    """

    ret = {
        "unicastRoutes": [unicast_route_to_dict(r) for r in route_db.unicastRoutes],
        "mplsRoutes": [mpls_route_to_dict(r) for r in route_db.mplsRoutes],
    }
    return ret


def print_routes_json(
    route_db_dict: Dict,
    prefixes: Optional[List[str]] = None,
    labels: Optional[List[int]] = None,
) -> None:
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
    nexthops_to_neighbor_names: Optional[Dict[bytes, str]] = None,
) -> None:
    """print the routes from Decision/Fib module"""

    if prefixes or not labels:
        print_unicast_routes(
            "Unicast Routes for {}".format(route_db.thisNodeName),
            route_db.unicastRoutes,
            prefixes=prefixes,
            nexthops_to_neighbor_names=nexthops_to_neighbor_names,
        )
    if labels or not prefixes:
        print_mpls_routes(
            "MPLS Routes for {}".format(route_db.thisNodeName),
            route_db.mplsRoutes,
            labels=labels,
        )


def find_adj_list_deltas(
    old_adj_list: Optional[Sequence[openr_types.Adjacency]],
    new_adj_list: Optional[Sequence[openr_types.Adjacency]],
    tags: Optional[Sequence[str]] = None,
) -> List:
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

    if old_adj_list is None:
        old_adj_list = set()
    if new_adj_list is None:
        new_adj_list = set()

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
        # pyre-fixme[6]: For 1st argument expected `Iterable[Tuple[str, typing.Any,
        #  None]]` but got `List[Tuple[str, Adjacency, Adjacency]]`.
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
        old_adj = object_to_dict(data[1]) if data[1] else None
        new_adj = object_to_dict(data[2]) if data[2] else None

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


def sprint_adj_delta(old_adj, new_adj) -> str:
    """given old and new adjacency, create a list of strings that summarize
    changes. If oldAdj is None, this function prints all attridutes of
    newAdj

    :param oldAdj Adjacency: can be None
    :param newAdj Adjacency: new

    :return str: table summarizing the change
    """
    assert new_adj is not None
    rows = []
    new_adj_dict = object_to_dict(new_adj)
    if old_adj is not None:
        old_adj_dict = object_to_dict(old_adj)
        for k in sorted(new_adj_dict.keys()):
            if old_adj_dict.get(k) != new_adj_dict.get(k):
                rows.append([k, old_adj_dict.get(k), "-->", new_adj_dict.get(k)])
    else:
        for k in sorted(new_adj_dict.keys()):
            rows.append([k, new_adj_dict[k]])
    return printing.render_horizontal_table(rows)


def sprint_pub_update(global_publication_db, key, value) -> str:
    """
    store new version and originatorId for a key in the global_publication_db
    return a string summarizing any changes in a publication from kv store
    """

    rows = []
    old_value = global_publication_db.get(key, kv_store_types_py.Value())

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
    global_prefix_db: Dict,
    prefix_db: Union[openr_types_py.PrefixDatabase, openr_types.PrefixDatabase],
    key: Optional[str] = None,
) -> None:
    """update the global prefix map with a single publication

    :param global_prefix_map map(node, set([str])): map of all prefixes
        in the network
    :param prefix_db PrefixDatabase: publication from single
        node
    """

    assert type(prefix_db) in [
        openr_types_py.PrefixDatabase,
        openr_types.PrefixDatabase,
    ]

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

    :param new_adj_db openr_types_py.AdjacencyDatabase: latest from kv store
    :param old_adj_db openr_types_py.AdjacencyDatabase: last one we had

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
    old_db_dict = thrift_to_dict(old_adj_db)
    old_db_dict.pop("adjacencies", None)
    old_db_dict.pop("perfEvents", None)
    new_db_dict = thrift_to_dict(new_adj_db)
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
    prefix_db: Union[openr_types_py.PrefixDatabase, openr_types.PrefixDatabase],
    key: Optional[str] = None,
) -> List[str]:
    """given serialzied prefixes for a single node, output the delta
        between those prefixes and global prefixes snapshot

    prefix could be entire prefix DB or per prefix key
    entire prefix DB: prefix:<node name>
    per prefix key: prefix:<node name>:<area>:<[IP addr/prefix len]

    :global_prefixes_db map(node, set([str])): global prefixes
    :prefix_db PrefixDatabase: latest from kv store

    :return [str]: the array of prefix strings
    """

    this_node_name = prefix_db.thisNodeName
    prev_prefixes = global_prefixes_db.get(this_node_name, set())

    added_prefixes = set()
    removed_prefixes = set()
    cur_prefixes = set()

    for prefix_entry in prefix_db.prefixEntries:
        cur_prefixes.add(ipnetwork.sprint_prefix(prefix_entry.prefix))

    # per prefix key format contains only one key, it can be an 'add' or 'delete'
    if key and re.match(Consts.PER_PREFIX_KEY_REGEX, key):
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
) -> kv_store_types_py.Publication:
    pub = None

    with get_openr_ctrl_client_py(host, cli_opts) as client:
        keyDumpParams = kv_store_types_py.KeyDumpParams()
        keyDumpParams.keys = [Consts.ALL_DB_MARKER]
        if area is None:
            pub = client.getKvStoreKeyValsFiltered(keyDumpParams)
        else:
            pub = client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)

    return pub


def build_nexthops(nexthops: List[str]) -> List[network_types.BinaryAddress]:
    """
    Convert nexthops in list of string to list of binaryAddress in thrift-python
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
        nexthop = nexthop(ifName=iface)
        nhs.append(nexthop)

    return nhs


def build_routes(
    prefixes: List[str], nexthops: List[str]
) -> List[network_types.UnicastRoute]:
    """
    Build list of UnicastRoute in thrift-python using prefixes and nexthops list
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
    routes: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
    route_type: str = "unicast",
) -> Dict[Any, List[str]]:
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
    routes: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
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
    lhs: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
    rhs: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
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
    #   1. dest for network_types_py.UnicastRoute
    #   2. topLabel for network_types_py.MplsRoute
    diff_keys = set(_lhs) - set(_rhs)
    for key in diff_keys:
        diff.append(_lhs[key])

    return diff


def prefixes_with_different_nexthops(
    lhs: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
    rhs: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
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
    routes_a: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
    routes_b: Union[
        List[network_types.UnicastRoute],
        List[network_types.MplsRoute],
    ],
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
        if is_color_output_supported():
            click.echo(click.style("PASS", bg="green", fg="black"))
        else:
            click.echo("PASS")
        print("{} and {} routing table match".format(*sources))
        return True, error_msg

    # Something failed.. report it
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


def validate_route_nexthops(routes, interfaces, sources, quiet: bool = False):
    """
    Validate between fib routes and lm interfaces

    :param routes: list network_types_py.UnicastRoute (structured routes)
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
                network_types_py.UnicastRoute(dest=route.dest, nextHops=v)
            )

    # if all good, then return early
    if not invalid_routes:
        if not quiet:
            if is_color_output_supported():
                click.echo(click.style("PASS", bg="green", fg="black"))
            else:
                click.echo("PASS")
            print("Route nexthop validation successful")
        return True, error_msg

    # Something failed.. report it
    if not quiet:
        if is_color_output_supported():
            click.echo(click.style("FAIL", bg="red", fg="black"))
        else:
            click.echo("FAIL")
        print("Route nexthop validation failed")
    # Output report per error type
    for err, route_db in invalid_routes.items():
        caption = "Error: {}".format(err)
        if not quiet:
            print_unicast_routes(caption, route_db)
        else:
            error_msg.append(caption)

    return False, error_msg


def mpls_action_to_str(
    mpls_action: Union[network_types_py.MplsAction, network_types.MplsAction]
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
        label_str = f" {'/'.join(str(l) for l in push_labels)}"
    return f"mpls {action_str}{label_str}"


def ip_nexthop_to_str(
    nextHop: Union[network_types_py.NextHopThrift, network_types.NextHopThrift],
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
        Sequence[network_types.UnicastRoute], List[network_types_py.UnicastRoute]
    ],
    prefixes: Optional[List[str]] = None,
    element_prefix: str = ">",
    element_suffix: str = "",
    filter_exact_match: bool = False,
    timestamp: bool = False,
    nexthops_to_neighbor_names: Optional[Dict[bytes, str]] = None,
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
        :param nexthops_to_neighbor_names:
            Use to print out friendly other node name rather than IP addresses
    """

    networks = None
    if prefixes:
        networks = [ipaddress.ip_network(p) for p in prefixes]

    route_strs = []
    for route in unicast_routes:
        entry = build_unicast_route(
            route,
            filter_for_networks=networks,
            nexthops_to_neighbor_names=nexthops_to_neighbor_names,
        )
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
    route: Union[network_types.UnicastRoute, network_types_py.UnicastRoute],
    filter_for_networks: Optional[
        List[Union[ipaddress.IPv4Network, ipaddress.IPv6Network]]
    ] = None,
    filter_exact_match: bool = False,
    nexthops_to_neighbor_names: Optional[Dict[bytes, str]] = None,
) -> Optional[Tuple[str, List[str]]]:
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


def print_mpls_routes(
    caption: str,
    mpls_routes: Union[
        List[network_types_py.MplsRoute], Sequence[network_types.MplsRoute]
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
    routes: List[network_types_py.UnicastRoute],
    prefixes: Optional[List[str]],
    mpls_routes: Optional[List[network_types_py.MplsRoute]],
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
            # pyre-fixme[16]: Item `int` of `Union[List[typing.Any], int, str]` has
            #  no attribute `append`.
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


async def get_areas_list(client: OpenrCtrlCppClient.Async) -> Set[str]:
    return {a.area_id for a in (await client.getRunningConfigThrift()).areas}


async def deduce_area(
    client: OpenrCtrlCppClient.Async,
    area: str,
    configured_areas: Optional[Set[str]] = None,
) -> str:
    """
    Deduce full area name from configured areas, if not ambiguous
    If configured areas is not provided, then get it from running config
    """
    areas = configured_areas or await get_areas_list(client)
    # exact match
    if area in areas:
        return area
    # substring match
    matches = [candidate for candidate in areas if area in candidate]
    if not matches:
        raise Exception(f"Invalid area {area}, configured areas: {', '.join(areas)}")
    elif len(matches) > 1:
        raise Exception(f"Ambiguous area {area}, found {', '.join(matches)}.")
    print(f"Operating on area: {matches[0]}")
    return matches[0]


# This API is used by commands that need one and only one
# area ID. For older images that don't support area feature, this API will
# return 'None'. If area ID is passed, API checks if it's valid and returns
# the same ID
async def get_area_id(client: OpenrCtrlCppClient.Async, area: str) -> str:
    # if no area is provided, return area in case only one area is configured
    areas = await get_areas_list(client)
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
    routes: Iterable[
        Union[
            ctrl_types.AdvertisedRouteDetail,
            ctrl_types.ReceivedRouteDetail,
        ]
    ],
    key_to_str_fn: Callable[[PrintAdvertisedTypes], Tuple[str, ...]],
    detailed: bool,
    tag_map: Optional[Dict[str, str]] = None,
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
             requirements=<min-nexthops> <tags?> <area-stack?>


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
            print_route_helper(rows, route, key_to_str_fn, detailed, markers, tag_map)
        rows.append("")

    print("\n".join(rows))


def print_advertised_routes(
    routes: Sequence[ctrl_types.AdvertisedRoute],
    key_to_str_fn: Callable[[PrintAdvertisedTypes], Tuple[str]],
    detailed: bool,
    tag_map: Optional[Dict[str, str]] = None,
) -> None:
    """
    Print postfilter advertised or rejected route.

    `key_to_str_fn` argument specifies the transformation of key attributes to tuple of
    strings

    @param:
        -tag_map: If present, translate the tag value using the map

    Output format
      > 10.0.0.0/8, entries=10, best-entries(*)=1, best-entry(@)
        [@*] source <key>
             forwarding algo=<fwd-algo> type=<fwd-type>
             metrics=<metrics>
             requirements=<min-nexthops> <tags?> <area-stack?>


    """

    rows = []
    # print header
    print_route_header(rows, detailed)

    for route in routes:
        # Create a title for the route
        rows.append(f"> {ipnetwork.sprint_prefix(route.route.prefix)}")
        print_route_helper(rows, route, key_to_str_fn, detailed, "{*@}", tag_map)
        rows.append("")

    print("\n".join(rows))


def print_route_header(rows: List[str], detailed: bool) -> None:
    """
    Helper function to construct print lines of header for advertised and received rooutes.
    """
    # Add marker information
    rows.append(
        "Markers: * - Best entries (used for forwarding), @ - Entry used to advertise across area"
    )
    if not detailed:
        rows.append(
            "Acronyms: SP - Source Preference, PP - Path Preference, D - Distance\n"
            "          MN - Min-Nexthops"
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
            "MN"
        )
        rows.append("")


def print_route_helper(
    rows: List[str],
    route: Union[
        ctrl_types.AdvertisedRoute,
        ctrl_types.ReceivedRoute,
    ],
    key_to_str_fn: Callable[[PrintAdvertisedTypes], Tuple[str, ...]],
    detailed: bool,
    markers: str,
    tag_map: Optional[Dict[str, str]] = None,
) -> None:
    """
    Construct print lines of advertised route, append to rows

    `key_to_str_fn` argument specifies the transformation of key attributes to tuple of
    strings
    `markers` argument specifies [@*] to indicate route is ecmp/best entry
    `tag_map` if present, map tag string to its name for readability

    Output format
        [@*] source <key>
             forwarding algo=<fwd-algo> type=<fwd-type>
             metrics=<metrics>
             requirements=<min-nexthops> <tags?> <area-stack?>


    """

    key, metrics = key_to_str_fn(route.key), route.route.metrics
    fwd_algo = route.route.forwardingAlgorithm.name
    fwd_type = route.route.forwardingType.name
    if detailed:
        rows.append(f"{markers} from {' '.join(key)}")
        rows.append(f"     Forwarding - algorithm: {fwd_algo}, type: {fwd_type}")
        rows.append(
            f"     Metrics - path-preference: {metrics.path_preference}"
            f", source-preference: {metrics.source_preference}"
            f", distance: {metrics.distance}"
            f", drained-path: {metrics.drain_metric}"
        )
        if route.route.minNexthop:
            rows.append(f"     Performance - min-nexthops: {route.route.minNexthop}")
        if route.route.weight:
            rows.append(f"     Misc - weight: {route.route.weight}")
        tag_map = tag_map if tag_map is not None else {}
        rows.append(
            f"     Tags - {', '.join(sorted([format_openr_tag(t,tag_map) for t in route.route.tags]))}"
        )
        rows.append(f"     Area Stack - {', '.join(route.route.area_stack)}")
        if (
            isinstance(route, ctrl_types.AdvertisedRoute)
            and hasattr(route, "hitPolicy")
            and route.hitPolicy
        ):
            rows.append(f"     Policy - {route.hitPolicy}")
        if isinstance(route, ctrl_types.AdvertisedRoute) and route.igpCost is not None:
            rows.append(f"     IGP Cost - {route.igpCost}")
    else:
        min_nexthop = (
            route.route.minNexthop if route.route.minNexthop is not None else "-"
        )
        rows.append(
            f"{markers:<2} {' '.join(key)[:36]:<36} "
            f"{fwd_algo:<12} {fwd_type:<8} "
            f"{metrics.source_preference:<6} "
            f"{metrics.path_preference:<6} "
            f"{metrics.distance:<6} "
            f"{min_nexthop}"
        )


def get_tag_to_name_map(config) -> Dict[str, str]:
    """
    Get mapping from tag_value to tag_name if it exists.
    e.g 65527:36706 -> FABRIC_POST_FSW_LOOP_AGG
    """
    try:
        tag_def = config["area_policies"]["definitions"]["openrTag"]["objects"]
    except KeyError:
        return {}
    return {v["tagSet"][0]: k for k, v in tag_def.items()}


def format_openr_tag(tag: str, tag_to_name_map: Dict[str, str]) -> str:
    return f"{tag_to_name_map.get(tag, '(NA)')}/{tag}"


async def adjs_nexthop_to_neighbor_name(
    client: OpenrCtrlCppClient.Async,
) -> Dict[bytes, str]:
    adj_dbs = await client.getDecisionAdjacenciesFiltered(
        ctrl_types.AdjacenciesFilter(selectAreas=None)
    )
    ips_to_node_names: Dict[bytes, str] = {}
    for adj_db in adj_dbs:
        for adj in adj_db.adjacencies:
            ips_to_node_names[adj.nextHopV6.addr] = adj.otherNodeName
            if adj.nextHopV4 != b"\x00\x00\x00\x00" and adj.nextHopV4 is not None:
                ips_to_node_names[adj.nextHopV4.addr] = adj.otherNodeName

    return ips_to_node_names
