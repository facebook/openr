#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import asyncio
import datetime
import hashlib
import ipaddress
import json
import re
import string
import sys
import time
from builtins import str
from collections import defaultdict
from collections.abc import Iterable
from itertools import combinations
from typing import (
    AbstractSet,
    Any,
    Callable,
    Dict,
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
import hexdump
import prettytable
import pytz
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.clients.openr_client import get_openr_ctrl_cpp_client
from openr.thrift.KvStore.thrift_types import (
    FilterOperator,
    InitializationEvent,
    KeyDumpParams,
    KeySetParams,
    KvStoreAreaSummary,
    KvStorePeerState,
    PeerSpec,
    Publication,
    Value,
)
from openr.thrift.Network import thrift_types as network_types
from openr.thrift.OpenrCtrl.thrift_types import StreamSubscriberType
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Types import thrift_types as openr_types
from openr.utils import ipnetwork, printing
from openr.utils.consts import Consts
from thrift.python.client import ClientType
from thrift.python.serializer import deserialize


class KvStoreCmdBase(OpenrCtrlCmd):
    def __init__(self, cli_opts: bunch.Bunch):
        super().__init__(cli_opts)
        self.areas: Set = set()

    def print_publication_delta(
        self,
        title: str,
        pub_update: List[str],
        sprint_db: str = "",
        timestamp=False,
    ) -> None:
        print(
            printing.render_vertical_table(
                [
                    [
                        "{}\n{}{}".format(
                            title,
                            pub_update,
                            "\n\n{}".format(sprint_db) if sprint_db else "",
                        )
                    ]
                ],
                timestamp=timestamp,
            )
        )

    def iter_publication(
        self,
        container: Any,
        publication: Any,
        nodes: set,
        parse_func: Callable[[Any, str], None],
    ) -> None:
        """
        parse dumped publication

        @param: container - Any: container to store the generated data
        @param: publication - Publication: the publication for parsing
        @param: nodes - set: the set of nodes for parsing
        @param: parse_func - function: the parsing function
        """

        for (key, value) in sorted(publication.keyVals.items(), key=lambda x: x[0]):
            reported_node_name = key.split(":")[1]
            if "all" not in nodes and reported_node_name not in nodes:
                continue

            parse_func(container, value)

    async def get_node_to_ips(
        self, client: OpenrCtrlCppClient.Async, area: Optional[str] = None
    ) -> Dict:
        """get the dict of all nodes to their IP in the network"""

        node_dict = {}
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        resp = Publication()
        if area is None:
            print(f"Error: Must specify one of the areas: {self.areas}")
            sys.exit(1)
        resp = await client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)

        prefix_maps = utils.collate_prefix_keys(resp.keyVals)
        for node, prefix_db in prefix_maps.items():
            node_dict[node] = self.get_node_ip(prefix_db)

        return node_dict

    def get_node_ip(self, prefix_db: openr_types.PrefixDatabase) -> Any:
        """get routable IP address of node from it's prefix database"""

        # First look for LOOPBACK prefix
        for prefix_entry in prefix_db.prefixEntries:
            if prefix_entry.type == network_types.PrefixType.LOOPBACK:
                return ipnetwork.sprint_addr(prefix_entry.prefix.prefixAddress.addr)

        # Else return None
        return None

    def get_area_id(self) -> str:
        if 1 != len(self.areas):
            print(f"Error: Must specify one of the areas: {self.areas}")
            sys.exit(1)
        (area,) = self.areas
        return area

    def print_peers(self, host_id: str, peers_list: Dict[str, Any]) -> None:
        """print the Kv Store peers"""

        caption = "{}'s peers".format(host_id)

        rows = []
        column_labels = [
            "Peer",
            "State",
            "Elapsed time",
            "Flaps",
            "Address",
            "Port",
            "Area",
        ]

        for area, peers in peers_list.items():
            for (peer, peerspec) in sorted(peers.items(), key=lambda x: x[0]):
                elapsed_time = ""
                flaps = ""
                if peerspec.stateElapsedTimeMs is not None:
                    elapsed_time = str(
                        datetime.timedelta(milliseconds=peerspec.stateElapsedTimeMs)
                    )
                if peerspec.flaps is not None:
                    flaps = peerspec.flaps
                row = [
                    peer,
                    peerspec.state.name,
                    elapsed_time,
                    flaps,
                    peerspec.peerAddr,
                    peerspec.ctrlPort,
                    area,
                ]

                rows.append(row)

        print(printing.render_horizontal_table(rows, column_labels, caption=caption))

    def print_kvstore_keys(
        self, resp: Dict[str, Publication], ttl: bool, json: bool
    ) -> None:
        """print keys from raw publication from KvStore"""

        # Export in json format if enabled
        if json:
            all_kv = {}
            for _, kv in resp.items():
                all_kv.update(kv.keyVals)

            # Force set value to None
            for k, v in all_kv.items():
                all_kv[k] = v(value=None)

            data = {}
            for k, v in all_kv.items():
                data[k] = utils.thrift_to_dict(v)
            print(utils.json_dumps(data))
            return

        rows = []
        db_bytes = 0
        num_keys = 0
        for area in resp:
            keyVals = resp[area].keyVals
            num_keys += len(keyVals)
            area_str = "N/A" if area is None else area
            for key, value in sorted(keyVals.items(), key=lambda x: x[0]):
                # 32 bytes comes from version, ttlVersion, ttl and hash which are i64
                bytes_value = value.value
                bytes_len = len(bytes_value if bytes_value is not None else b"")
                kv_size = 32 + len(key) + len(value.originatorId) + bytes_len
                db_bytes += kv_size

                hash_num = value.hash
                hash_offset = "+" if hash_num is not None and hash_num > 0 else ""

                row = [
                    key,
                    value.originatorId,
                    value.version,
                    f"{hash_offset}{value.hash:x}",
                    printing.sprint_bytes(kv_size),
                    area_str,
                ]
                if ttl:
                    ttlStr = (
                        "Inf"
                        if value.ttl == Consts.CONST_TTL_INF
                        else str(datetime.timedelta(milliseconds=value.ttl))
                    )
                    row.append(f"{ttlStr} - {value.ttlVersion}")
                rows.append(row)

        db_bytes_str = printing.sprint_bytes(db_bytes)
        caption = f"KvStore Data - {num_keys} keys, {db_bytes_str}"
        column_labels = ["Key", "Originator", "Ver", "Hash", "Size", "Area"]
        if ttl:
            column_labels = column_labels + ["TTL - Ver"]

        print(printing.render_horizontal_table(rows, column_labels, caption))

    async def fetch_peers(
        self, client: OpenrCtrlCppClient.Async, area=None
    ) -> Mapping[str, PeerSpec]:
        """
        Fetch a dictionary of {peer name : peer spec} via thrift call
        """

        if area:
            return await client.getKvStorePeersArea(area)

        return await client.getKvStorePeers()

    async def fetch_keyvals(
        self,
        client: OpenrCtrlCppClient.Async,
        areas: Set[Any],
        keyDumpParams: KeyDumpParams,
    ) -> Dict[str, Publication]:
        """
        Fetch the keyval publication for each area specified in areas via thrift call
        Returned as a dict {area : publication}
        If keyDumpParams is specified, returns keyvals filtered accordingly
        """

        area_to_publication_dict = {}
        for area in areas:
            area_to_publication_dict[area] = await client.getKvStoreKeyValsFilteredArea(
                keyDumpParams, area
            )
        return area_to_publication_dict


class KvStoreWithInitAreaCmdBase(KvStoreCmdBase):
    async def _init_area(self, client: OpenrCtrlCppClient.Async) -> None:
        # area provided, validate or deduce
        if hasattr(self.cli_opts, "area"):
            if self.cli_opts.area != "":
                self.areas = {await utils.deduce_area(client, self.cli_opts.area)}
                return
        # no area provided, return all areas
        self.areas = await utils.get_areas_list(client)

    # @override
    def run(self, *args, **kwargs) -> int:
        """
        run method that invokes _run with client and arguments
        """

        async def _wrapper() -> int:
            ret_val: Optional[int] = 0
            async with get_openr_ctrl_cpp_client(
                self.host,
                self.cli_opts,
                client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
            ) as client:
                await self._init_area(client)
                ret_val = await self._run(client, *args, **kwargs)
            if ret_val is None:
                ret_val = 0
            return ret_val

        return asyncio.run(_wrapper())


class PrefixesCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        nodes: set,
        json: bool,
        prefix: str = "",
        client_type: str = "",
        *args,
        **kwargs,
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(prefix=Consts.PREFIX_DB_MARKER)
        area_kv = {}
        for area in self.areas:
            resp = await client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)
            area_kv[area] = resp
        self.print_prefix(area_kv, nodes, json, prefix, client_type)

    def print_prefix(
        self,
        resp: Dict[str, Publication],
        nodes: set,
        json: bool,
        prefix: str,
        client_type: str,
    ):
        key_vals = {}
        for _, val in resp.items():
            key_vals.update(val.keyVals)
        all_kv = Publication(keyVals=key_vals)
        if json:
            utils.print_prefixes_json(
                all_kv, nodes, prefix, client_type, self.iter_publication
            )
        else:
            utils.print_prefixes_table(
                all_kv, nodes, prefix, client_type, self.iter_publication
            )


class KeysCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        json: bool,
        prefix: Any,
        originator: Any = None,
        ttl: bool = False,
        *args,
        **kwargs,
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(
            prefix, {originator} if originator else None
        )

        area_kv = {}
        for area in self.areas:
            resp = await client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)
            area_kv[area] = resp

        self.print_kvstore_keys(area_kv, ttl, json)


class KeyValsCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        keys: List[str],
        *args,
        **kwargs,
    ) -> None:
        for area in self.areas:
            resp = await client.getKvStoreKeyValsArea(keys, area)
            if len(resp.keyVals):
                self.print_kvstore_values(resp, area)

    def deserialize_kvstore_publication(self, key, value):
        """classify kvstore prefix and return the corresponding deserialized obj"""

        options = {
            Consts.PREFIX_DB_MARKER: openr_types.PrefixDatabase,
            Consts.ADJ_DB_MARKER: openr_types.AdjacencyDatabase,
        }

        prefix_type = key.split(":")[0] + ":"
        if prefix_type in options.keys():
            return deserialize(options[prefix_type], value.value)
        else:
            return None

    def print_kvstore_values(
        self,
        resp: Publication,
        area: Optional[str] = None,
    ) -> None:
        """print values from raw publication from KvStore"""

        rows = []
        for key, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            val = self.deserialize_kvstore_publication(key, value)
            if not val:
                if isinstance(value.value, Iterable) and all(
                    isinstance(c, str) and c in string.printable for c in value.value
                ):
                    val = value.value
                else:
                    val = hexdump.hexdump(value.value, "return")

            ttl = "INF" if value.ttl == Consts.CONST_TTL_INF else value.ttl
            rows.append(
                [
                    "key: {}\n  version: {}\n  originatorId: {}\n  "
                    "ttl: {}\n  ttlVersion: {}\n  value:\n    {}".format(
                        key,
                        value.version,
                        value.originatorId,
                        ttl,
                        value.ttlVersion,
                        val,
                    )
                ]
            )

        area = f"in area {area}" if area is not None else ""
        caption = f"Dump key-value pairs in KvStore {area}"
        print(printing.render_vertical_table(rows, caption=caption))


class NodesCmd(KvStoreWithInitAreaCmdBase):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        *args,
        **kwargs,
    ) -> None:
        key_vals = {}
        node_area = {}
        nodes = set()
        for area in self.areas:
            prefix_keys = await client.getKvStoreKeyValsFilteredArea(
                self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER), area
            )
            key_vals.update(prefix_keys.keyVals)
            adj_keys = await client.getKvStoreKeyValsFilteredArea(
                self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER), area
            )
            host_id = await client.getMyNodeName()
            node_set = self.get_connected_nodes(adj_keys, host_id)
            # save area associated with each node
            for node in node_set:
                node_area[node] = area
            nodes.update(node_set)

        all_kv = Publication(keyVals=key_vals)

        # pyre-fixme[61]: `host_id` may not be initialized here.
        self.print_kvstore_nodes(nodes, all_kv, host_id, node_area)

    def get_connected_nodes(self, adj_keys: Publication, node_id: str) -> Set[str]:
        """
        Build graph of adjacencies and return list of connected node from
        current node-id
        """
        import networkx as nx

        edges = set()
        graph = nx.Graph()
        for adj_value in adj_keys.keyVals.values():
            if adj_value.value:
                adj_db = deserialize(openr_types.AdjacencyDatabase, adj_value.value)
            else:
                adj_db = openr_types.AdjacencyDatabase()
            graph.add_node(adj_db.thisNodeName)
            for adj in adj_db.adjacencies:
                # Add edge only when we see the reverse side of it.
                if (adj.otherNodeName, adj_db.thisNodeName, adj.otherIfName) in edges:
                    graph.add_edge(adj.otherNodeName, adj_db.thisNodeName)
                    continue
                edges.add((adj_db.thisNodeName, adj.otherNodeName, adj.ifName))
        # pyre-ignore[16]
        return nx.node_connected_component(graph, node_id)

    def print_kvstore_nodes(
        self,
        connected_nodes: Set[str],
        prefix_keys: Publication,
        host_id: str,
        node_area: Optional[Dict[str, str]] = None,
    ) -> None:
        """
        Print kvstore nodes information. Their loopback and reachability
        information.
        """

        def _parse_loopback_addrs(addrs, value):
            v4_addrs = addrs["v4"]
            v6_addrs = addrs["v6"]
            prefix_db = deserialize(openr_types.PrefixDatabase, value.value)

            for prefixEntry in prefix_db.prefixEntries:
                p = prefixEntry.prefix
                if prefixEntry.type != network_types.PrefixType.LOOPBACK:
                    continue

                if len(p.prefixAddress.addr) == 16 and p.prefixLength == 128:
                    v6_addrs[prefix_db.thisNodeName] = ipnetwork.sprint_prefix(p)

                if len(p.prefixAddress.addr) == 4 and p.prefixLength == 32:
                    v4_addrs[prefix_db.thisNodeName] = ipnetwork.sprint_prefix(p)

        # Extract loopback addresses
        addrs = {"v4": {}, "v6": {}}
        self.iter_publication(addrs, prefix_keys, {"all"}, _parse_loopback_addrs)

        # Create rows to print
        rows = []
        for node in set(list(addrs["v4"].keys()) + list(addrs["v6"].keys())):
            marker = "* " if node == host_id else "> "
            loopback_v4 = addrs["v4"].get(node, "N/A")
            loopback_v6 = addrs["v6"].get(node, "N/A")
            area_str = node_area.get(node, "N/A") if node_area is not None else "N/A"
            rows.append(
                [
                    f"{marker}{node}",
                    loopback_v6,
                    loopback_v4,
                    "Reachable" if node in connected_nodes else "Unreachable",
                    area_str,
                ]
            )

        label = ["Node", "V6-Loopback", "V4-Loopback", "Status", "Area"]

        print(printing.render_horizontal_table(rows, label))


class Areas(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        in_json: bool,
        *args,
        **kwargs,
    ) -> None:
        if in_json:
            print(json.dumps(list(self.areas)))
        else:
            print(f"Areas configured: {self.areas}")


class ShowAdjNodeCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        nodes: set,
        node: Any,
        interface: Any,
        *args,
        **kwargs,
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)

        key_vals = {}
        for area in self.areas:
            publication = await client.getKvStoreKeyValsFilteredArea(
                keyDumpParams, area
            )
            key_vals.update(publication.keyVals)

        resp = Publication(keyVals=key_vals)

        self.printAdjNode(resp, nodes, node, interface)

    def printAdjNode(self, publication, nodes, node, interface):
        adjs_map = utils.adj_dbs_to_dict(
            publication, nodes, True, self.iter_publication
        )
        utils.print_adjs_table(adjs_map, node, interface)


class KvCompareCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        nodes_in: str,
        *args,
        **kwargs,
    ) -> None:
        area = self.get_area_id()

        all_nodes_to_ips = await self.get_node_to_ips(client, area)
        if nodes_in:
            nodes = set(nodes_in.strip().split(","))
            if "all" in nodes:
                nodes = set(all_nodes_to_ips.keys())
            host_id = await client.getMyNodeName()
            if host_id in nodes:
                nodes.remove(host_id)

            keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ALL_DB_MARKER)
            pub = await client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips, area)
            for node in kv_dict:
                self.compare(pub.keyVals, kv_dict[node], host_id, node)
        else:
            nodes = set(all_nodes_to_ips.keys())
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips, area)
            for our_node, other_node in combinations(kv_dict.keys(), 2):
                self.compare(
                    kv_dict[our_node], kv_dict[other_node], our_node, other_node
                )

    def compare(self, our_kvs, other_kvs, our_node, other_node):
        """print kv delta"""

        print(
            printing.caption_fmt(
                "kv-compare between {} and {}".format(our_node, other_node)
            )
        )

        # for comparing version and id info
        our_kv_pub_db = {}
        for key, value in our_kvs.items():
            our_kv_pub_db[key] = (value.version, value.originatorId)

        for key, value in sorted(our_kvs.items()):
            other_val = other_kvs.get(key, None)
            if other_val is None:
                self.print_key_delta(key, our_node)

            elif (
                key.startswith(Consts.PREFIX_DB_MARKER)
                or key.startswith(Consts.ADJ_DB_MARKER)
                or other_val.value != value.value
            ):
                self.print_db_delta(key, our_kv_pub_db, value, other_val)

        for key, _ in sorted(other_kvs.items()):
            ourVal = our_kvs.get(key, None)
            if ourVal is None:
                self.print_key_delta(key, other_node)

    def print_db_delta(self, key, our_kv_pub_db, value, other_val):
        """print db delta"""

        if key.startswith(Consts.PREFIX_DB_MARKER):
            prefix_db = deserialize(openr_types.PrefixDatabase, value.value)
            other_prefix_db = deserialize(openr_types.PrefixDatabase, other_val.value)
            other_prefix_set = {}
            utils.update_global_prefix_db(other_prefix_set, other_prefix_db)
            lines = utils.sprint_prefixes_db_delta(other_prefix_set, prefix_db)

        elif key.startswith(Consts.ADJ_DB_MARKER):
            adj_db = deserialize(openr_types.AdjacencyDatabase, value.value)
            other_adj_db = deserialize(openr_types.AdjacencyDatabase, value.value)
            lines = utils.sprint_adj_db_delta(adj_db, other_adj_db)

        else:
            lines = None

        if lines != []:
            self.print_publication_delta(
                "Key: {} difference".format(key),
                utils.sprint_pub_update(our_kv_pub_db, key, other_val),
                "\n".join(lines) if lines else "",
            )

    def print_key_delta(self, key, node):
        """print key delta"""

        print(
            printing.render_vertical_table(
                [["key: {} only in {} kv store".format(key, node)]]
            )
        )

    def dump_nodes_kvs(
        self, nodes: set, all_nodes_to_ips: Dict, area: Optional[str] = None
    ):
        """get the kvs of a set of nodes"""

        kv_dict = {}
        for node in nodes:
            node_ip = all_nodes_to_ips.get(node, node)
            kv = utils.dump_node_kvs(self.cli_opts, node_ip, area)
            if kv is not None:
                kv_dict[node] = kv.keyVals
                print("dumped kv from {}".format(node))
        return kv_dict


class PeersCmd(KvStoreWithInitAreaCmdBase):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        *args,
        **kwargs,
    ) -> None:
        host_id = await client.getMyNodeName()

        area_to_peers_dict = {}

        for area in self.areas:
            area_to_peers_dict[area] = await self.fetch_peers(client, area=area)

        self.print_peers(host_id, area_to_peers_dict)


class EraseKeyCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        key: str,
        *args,
        **kwargs,
    ) -> None:
        area = self.get_area_id()
        publication = await client.getKvStoreKeyValsArea([key], area)
        keyVals = dict(publication.keyVals)

        if key not in keyVals:
            print("Error: Key {} not found in KvStore.".format(key))
            sys.exit(1)

        # Get and modify the key
        val = keyVals.get(key)

        newVal = Value(
            version=getattr(val, "version", 0),
            originatorId=getattr(val, "originatorId", ""),
            hash=getattr(val, "hash", 0),
            value=None,
            ttl=256,  # set new ttl to 256ms (its decremented 1ms on every hop)
            ttlVersion=getattr(val, "ttlVersion", 0) + 1,  # bump up ttl version
        )

        print(keyVals)
        keyVals[key] = newVal

        await client.setKvStoreKeyVals(KeySetParams(keyVals=keyVals), area)

        print("Success: key {} will be erased soon from all KvStores.".format(key))


class SetKeyCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        key: str,
        value: Union[bytes, str],
        originator: str,
        version: Any,
        ttl: int,
        *args,
        **kwargs,
    ) -> None:
        area = self.get_area_id()

        if version is None:
            # Retrieve existing Value from KvStore
            publication = None
            if area is None:
                publication = await client.getKvStoreKeyVals([key])
            else:
                publication = await client.getKvStoreKeyValsArea([key], area)
            if key in publication.keyVals:
                existing_val = publication.keyVals.get(key)
                curr_version = getattr(existing_val, "version", 0)
                print(
                    "Key {} found in KvStore w/ version {}. Overwriting with"
                    " higher version ...".format(key, curr_version)
                )
                version = curr_version + 1
            else:
                version = 1

        if isinstance(value, str):
            value = value.encode()

        val = Value(
            version=version,
            originatorId=originator,
            value=value,
            ttl=ttl,
            ttlVersion=1,
        )

        # Advertise publication back to KvStore
        keyVals = {key: val}
        await client.setKvStoreKeyVals(KeySetParams(keyVals=keyVals), area)
        print(
            "Success: Set key {} with version {} and ttl {} successfully"
            " in KvStore. This does not guarantee that value is updated"
            " in KvStore as old value can be persisted back".format(
                key,
                val.version,
                val.ttl if val.ttl != Consts.CONST_TTL_INF else "infinity",
            )
        )


class KvSignatureCmd(KvStoreWithInitAreaCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        prefix: str,
        *args,
        **kwargs,
    ) -> None:
        area = self.get_area_id()
        keyDumpParams = self.buildKvStoreKeyDumpParams(prefix)
        resp = None
        if area is None:
            resp = await client.getKvStoreHashFiltered(keyDumpParams)
        else:
            resp = await client.getKvStoreHashFilteredArea(keyDumpParams, area)

        signature = hashlib.sha256()
        for _, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            signature.update(str(value.hash).encode("utf-8"))

        print("sha256: {}".format(signature.hexdigest()))


class SnoopCmd(KvStoreCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        delta: bool,
        ttl: bool,
        regexes: Optional[List[str]],
        duration: int,
        originator_ids: Optional[AbstractSet[str]],
        match_all: bool,
        select_areas: Set[str],
        print_initial: bool,
        *args,
        **kwargs,
    ) -> None:
        kvDumpParams = KeyDumpParams(
            ignoreTtl=not ttl,
            keys=regexes,
            originatorIds=originator_ids,
            oper=FilterOperator.AND if match_all else FilterOperator.OR,
        )

        print("Retrieving and subcribing KvStore ... ")
        (snapshot, updates) = await client.subscribeAndGetAreaKvStores(
            kvDumpParams,
            select_areas,
        )
        global_dbs = self.process_snapshot(snapshot)
        if print_initial:
            for pub in snapshot:
                self.print_delta(pub, ttl, delta, global_dbs[pub.area])
        print(
            f"\nSnooping on areas {','.join(global_dbs.keys())}. Magic begins here ... \n"
        )

        start_time = time.time()
        awaited_updates = None
        while True:
            # Break if it is time
            if duration > 0 and time.time() - start_time > duration:
                break

            # Await for an update
            if not awaited_updates:
                awaited_updates = [updates.__anext__()]
            # pyre-fixme[6]: For 1st argument expected `Iterable[Variable[_FT (bound
            #  to Future[typing.Any])]]` but got `Union[List[Awaitable[Publication]],
            #  Set[typing.Any]]`.
            done, awaited_updates = await asyncio.wait(awaited_updates, timeout=1)
            if not done:
                continue
            else:
                msg = await done.pop()

            if msg.area in global_dbs:
                self.print_expired_keys(msg, global_dbs[msg.area])
                self.print_delta(msg, ttl, delta, global_dbs[msg.area])
            else:
                print(f"ERROR: got publication for unexpected area: {msg.area}")

    def print_expired_keys(self, msg: Publication, global_dbs: Dict):
        rows = []
        if len(msg.expiredKeys):
            print("Traversal List: {}".format(msg.nodeIds))

        for key in msg.expiredKeys:
            rows.append(["Key: {} got expired".format(key)])

            # Delete key from global DBs
            global_dbs["publications"].pop(key, None)
            if key.startswith(Consts.ADJ_DB_MARKER):
                global_dbs["adjs"].pop(key.split(":")[1], None)

            if key.startswith(Consts.PREFIX_DB_MARKER):
                prefix_match = re.match(Consts.PER_PREFIX_KEY_REGEX, key)
                # in case of per prefix key expire, the prefix DB entry does not
                # contain any prefixes. The prefix must be constructed from the
                # key. Update the prefix set of the corresponding node.
                if prefix_match:
                    prefix_set = set()
                    addr_str = prefix_match.group("ipaddr")
                    prefix_len = prefix_match.group("plen")
                    prefix_set.add("{}/{}".format(addr_str, prefix_len))
                    node_prefix_set = global_dbs["prefixes"][prefix_match.group("node")]
                    node_prefix_set = node_prefix_set - prefix_set
                else:
                    global_dbs["prefixes"].pop(key.split(":")[1], None)
        if rows:
            print(printing.render_vertical_table(rows, timestamp=True))

    def print_delta(
        self,
        msg: Publication,
        ttl: bool,
        delta: bool,
        global_dbs: Dict,
    ):

        for key, value in msg.keyVals.items():
            if value.value is None:
                print("Traversal List: {}".format(msg.nodeIds))
                self.print_publication_delta(
                    f"Key: {key}, ttl update",
                    [f"ttl: {value.ttl}, ttlVersion: {value.ttlVersion}"],
                    timestamp=True,
                )
                continue

            if key.startswith(Consts.ADJ_DB_MARKER):
                self.print_adj_delta(
                    key,
                    value,
                    delta,
                    global_dbs["adjs"],
                    global_dbs["publications"],
                )
                continue

            if key.startswith(Consts.PREFIX_DB_MARKER):
                self.print_prefix_delta(
                    key,
                    value,
                    delta,
                    global_dbs["prefixes"],
                    global_dbs["publications"],
                )
                continue

            print("Traversal List: {}".format(msg.nodeIds))
            self.print_publication_delta(
                "Key: {} update".format(key),
                # pyre-fixme[6]: For 2nd param expected `List[str]` but got `str`.
                utils.sprint_pub_update(global_dbs["publications"], key, value),
                timestamp=True,
            )

    def print_prefix_delta(
        self,
        key: str,
        value: Value,
        delta: bool,
        global_prefix_db: Dict,
        global_publication_db: Dict,
    ):
        if value.value:
            prefix_db = deserialize(
                openr_types.PrefixDatabase,
                value.value,
            )
        else:
            prefix_db = openr_types.PrefixDatabase()

        if delta:
            lines = "\n".join(
                utils.sprint_prefixes_db_delta(global_prefix_db, prefix_db, key)
            )
        else:
            lines = utils.sprint_prefixes_db_full(prefix_db)

        if lines:
            self.print_publication_delta(
                "{}'s prefixes".format(prefix_db.thisNodeName),
                # pyre-fixme[6]: For 2nd param expected `List[str]` but got `str`.
                utils.sprint_pub_update(global_publication_db, key, value),
                lines,
                timestamp=True,
            )

        utils.update_global_prefix_db(global_prefix_db, prefix_db, key)

    def print_adj_delta(
        self,
        key: str,
        value: Value,
        delta: bool,
        global_adj_db: Dict,
        global_publication_db: Dict,
    ):
        if value.value:
            new_adj_db = deserialize(openr_types.AdjacencyDatabase, value.value)
        else:
            new_adj_db = openr_types.AdjacencyDatabase()
        if delta:
            old_adj_db = global_adj_db.get(new_adj_db.thisNodeName, None)
            if old_adj_db is None:
                lines = "ADJ_DB_ADDED: {}\n".format(
                    new_adj_db.thisNodeName
                ) + utils.sprint_adj_db_full(global_adj_db, new_adj_db, False)
            else:
                lines = utils.sprint_adj_db_delta(new_adj_db, old_adj_db)
                lines = "\n".join(lines)
        else:
            lines = utils.sprint_adj_db_full(global_adj_db, new_adj_db, False)

        if lines:
            self.print_publication_delta(
                "{}'s adjacencies".format(new_adj_db.thisNodeName),
                # pyre-fixme[6]: For 2nd param expected `List[str]` but got `str`.
                utils.sprint_pub_update(global_publication_db, key, value),
                lines,
                timestamp=True,
            )

        utils.update_global_adj_db(global_adj_db, new_adj_db)

    def process_snapshot(self, resp: Sequence[Publication]) -> Dict:
        snapshots = {}
        print("Processing initial snapshots...")
        for pub in resp:
            global_dbs = bunch.Bunch(
                {
                    "prefixes": {},
                    "adjs": {},
                    "publications": {},  # map(key -> Value)
                }
            )

            # Populate global_dbs
            global_dbs.prefixes = utils.build_global_prefix_db(pub)
            global_dbs.adjs = utils.build_global_adj_db(pub)
            for key, value in pub.keyVals.items():
                global_dbs.publications[key] = value
            snapshots[pub.area] = global_dbs
            print(f"Loaded {len(pub.keyVals)} initial key-values for area {pub.area}")
        print("Done.")
        return snapshots


class SummaryCmd(KvStoreWithInitAreaCmdBase):
    def _get_summary_stats_template(self, area: str = "") -> List[Dict[str, Any]]:
        if area != "":
            title = " Stats for Area " + area
            area = "." + area
        else:
            title = "Global Summary Stats"
        return [
            {
                "title": title,
                "counters": [],
                "stats": [
                    ("  Sent Publications", f"kvstore.sent_publications{area}.count"),
                    ("  Sent KeyVals", f"kvstore.sent_key_vals{area}.sum"),
                    (
                        "  Rcvd Publications",
                        f"kvstore.received_publications{area}.count",
                    ),
                    ("  Rcvd KeyVals", f"kvstore.received_key_vals{area}.sum"),
                    ("  Updates KeyVals", f"kvstore.updated_key_vals{area}.sum"),
                ],
            },
        ]

    def _get_area_str(self) -> str:
        s = "s" if len(self.areas) != 1 else ""
        return f", {str(len(self.areas))} configured area{s}"

    def _get_bytes_str(self, bytes_count: int) -> str:
        if bytes_count < 1024:
            return "{} Bytes".format(bytes_count)
        elif bytes_count < 1024 * 1024:
            return "{:.2f}KB".format(bytes_count / 1024)
        else:
            return "{:.2f}MB".format(bytes_count / 1024 / 1024)

    def _get_peer_state_output(self, peersMap: Mapping[str, PeerSpec]) -> str:
        # form a list of peer state value for easy counting, for display
        states = [peer.state for peer in peersMap.values()]
        return (
            f" {states.count(KvStorePeerState.INITIALIZED)} Initialized,"
            f" {states.count(KvStorePeerState.SYNCING)} Syncing,"
            f" {states.count(KvStorePeerState.IDLE)} Idle"
        )

    def _get_area_summary(self, s: KvStoreAreaSummary) -> str:
        return (
            f"\n"
            f">> Area - {s.area}\n"
            f"   Peers: {len(s.peersMap)} Total - {self._get_peer_state_output(s.peersMap)}\n"
            f"   Database: {s.keyValsCount} KVs, {self._get_bytes_str(s.keyValsBytes)}"
        )

    def _get_global_summary(
        self, summaries: Sequence[KvStoreAreaSummary]
    ) -> KvStoreAreaSummary:
        peers_map = {}
        # create a map of unique total peers for this node
        for s in summaries:
            for peer, peerSpec in s.peersMap.items():
                # if the same peer appears in multiple areas, and in a different state, then replace with lower state
                if peer not in peers_map or peers_map[peer].state > peerSpec.state:
                    peers_map[peer] = peerSpec

        return KvStoreAreaSummary(
            area="ALL" + self._get_area_str(),
            # peersMap's type: Dict[str, PeerSpec]
            peersMap=peers_map,
            # count total key/value pairs across all areas
            keyValsCount=sum(s.keyValsCount for s in summaries),
            # count total size (in bytes) of KvStoreDB across all areas
            keyValsBytes=sum(s.keyValsBytes for s in summaries),
        )

    async def _print_summarized_output(
        self,
        client: OpenrCtrlCppClient.Async,
        summaries: Sequence[KvStoreAreaSummary],
        input_areas: Set[str],
    ) -> None:
        allFlag: bool = False
        # if no area(s) filter specified in CLI, then get all configured areas
        if len(input_areas) == 0:
            input_areas = set(self.areas)
            allFlag = True

        # include global summary, if no area(s) filter specified in CLI
        if allFlag:
            print(self._get_area_summary(self._get_global_summary(summaries)))
            self.print_stats(
                self._get_summary_stats_template(), await client.getCounters()
            )

        # build a list of per-area (filtered) summaries first
        for s in (s for s in summaries if s.area in input_areas):
            print(self._get_area_summary(s))
            self.print_stats(
                self._get_summary_stats_template(s.area), await client.getCounters()
            )

    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        input_areas: Set[str],
        *args,
        **kwargs,
    ) -> None:
        areaSet = set(self.areas)
        # get per-area Summary list from KvStore for all areas
        summaries = await client.getKvStoreAreaSummary(areaSet)
        # build summarized output from (filtered) summaries, and print it
        await self._print_summarized_output(client, summaries, input_areas)
        print()


def ip_key(ip: object):
    # pyre-fixme[6]: For 1st param expected `Union[bytes, int, IPv4Address,
    #  IPv4Interface, IPv4Network, IPv6Address, IPv6Interface, IPv6Network, str]` but
    #  got `object`.
    net = ipaddress.ip_network(ip)
    return (net.version, net.network_address, net.prefixlen)


def convertTime(intTime) -> str:
    formatted_time = datetime.datetime.fromtimestamp(intTime / 1000)
    timezone = pytz.timezone("US/Pacific")
    formatted_time = timezone.localize(formatted_time)
    formatted_time = formatted_time.strftime("%Y-%m-%d %H:%M:%S.%f %Z")
    return formatted_time


class StreamSummaryCmd(KvStoreCmdBase):
    def get_subscriber_row(self, stream_session_info):
        """
        Takes StreamSubscriberInfo from thrift and returns list[str] (aka row)
        representing the subscriber
        """

        uptime = "unknown"
        last_msg_time = "unknown"
        if (
            stream_session_info.uptime is not None
            and stream_session_info.last_msg_sent_time is not None
        ):
            uptime_str = str(
                datetime.timedelta(milliseconds=stream_session_info.uptime)
            )
            last_msg_time_str = convertTime(stream_session_info.last_msg_sent_time)
            uptime = uptime_str.split(".")[0]
            last_msg_time = last_msg_time_str

        row = [
            stream_session_info.subscriber_id,
            uptime,
            stream_session_info.total_streamed_msgs,
            last_msg_time,
        ]
        return row

    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        *args,
        **kwargs,
    ) -> None:

        subscribers = await client.getSubscriberInfo(StreamSubscriberType.KVSTORE)

        # Prepare the table
        columns = [
            "SubscriberID",
            "Uptime",
            "Total Messages",
            "Time of Last Message",
        ]
        table = ""
        table = prettytable.PrettyTable(columns)
        table.set_style(prettytable.PLAIN_COLUMNS)
        table.align = "l"
        table.left_padding_width = 0
        table.right_padding_width = 2

        for subscriber in sorted(subscribers, key=lambda x: ip_key(x.subscriber_id)):
            table.add_row(self.get_subscriber_row(subscriber))

        # Print header
        print("KvStore stream summary information for subscribers\n")

        if not subscribers:
            print("No subscribers available")

        # Print the table body
        print(table)


class ValidateCmd(KvStoreWithInitAreaCmdBase):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        *args,
        **kwargs,
    ) -> bool:

        is_pass = True

        # Get data
        openr_config = await client.getRunningConfigThrift()

        area_to_peers_dict = {}
        for area in self.areas:
            area_to_peers_dict[area] = await self.fetch_peers(client, area=area)

        keyDumpParams = self.buildKvStoreKeyDumpParams(
            originator_ids={openr_config.node_name}
        )
        area_to_publication_dict = await self.fetch_keyvals(
            client, self.areas, keyDumpParams
        )
        configured_ttl = openr_config.kvstore_config.key_ttl_ms

        initialization_events = await client.getInitializationEvents()

        # Run validation checks
        (
            is_adj_advertised,
            is_prefix_advertised,
        ) = self._validate_local_key_advertisement(area_to_publication_dict)

        is_pass = is_pass and is_adj_advertised and is_prefix_advertised

        invalid_peers = self._validate_peer_state(area_to_peers_dict)

        is_pass = is_pass and (len(invalid_peers) == 0)

        # Refetch the keys to get updated ttl
        keyDumpParams = self.buildKvStoreKeyDumpParams()
        area_to_invalid_keys = self._validate_key_ttl(
            await self.fetch_keyvals(client, self.areas, keyDumpParams), configured_ttl
        )

        is_pass = is_pass and (len(area_to_invalid_keys) == 0)

        init_is_pass, init_err_msg_str = self.validate_init_event(
            initialization_events,
            InitializationEvent.KVSTORE_SYNCED,
        )

        is_pass = is_pass and init_is_pass

        # Render validation results
        self._print_local_node_advertising_check(
            is_adj_advertised, is_prefix_advertised
        )
        self._print_peer_state_check(invalid_peers, openr_config.node_name)

        self._print_key_ttl_check(area_to_invalid_keys, configured_ttl)

        self.print_initialization_event_check(
            init_is_pass,
            init_err_msg_str,
            InitializationEvent.KVSTORE_SYNCED,
            "KvStore",
        )

        return is_pass

    def _validate_local_key_advertisement(
        self, area_to_publication_dict: Dict[str, Publication]
    ) -> Tuple[bool, bool]:
        """
        Checks if the local node is advertising atleast one adjacency key and
        one prefix key to the kvstore. Returns a boolean for each key type.
        """

        keys = []
        for _, publication in area_to_publication_dict.items():
            keys.extend(list(publication.keyVals.keys()))

        is_adj_advertised = False
        is_prefix_advertised = False
        for key in keys:
            if is_prefix_advertised and is_adj_advertised:
                break
            if re.match(Consts.PREFIX_DB_MARKER, key):
                is_prefix_advertised = True
            if re.match(Consts.ADJ_DB_MARKER, key):
                is_adj_advertised = True

        return is_adj_advertised, is_prefix_advertised

    def _validate_peer_state(
        self, area_to_peers: Dict[str, Dict[str, PeerSpec]]
    ) -> Dict[str, Dict[str, PeerSpec]]:
        """
        Checks if all peers are in INITIALIZED state,
        returns a dictionary of {area : peersmap of peers} which are not in
        INITIALIZED state.
        """

        invalid_peers = defaultdict(dict)
        for (area, peers) in area_to_peers.items():
            for (peer_name, peer_spec) in peers.items():
                if peer_spec.state != KvStorePeerState.INITIALIZED:
                    invalid_peers[area][peer_name] = peer_spec

        return dict(invalid_peers)

    def _validate_key_ttl(
        self,
        publications: Dict[str, Publication],
        ttl: int,
        threshold: float = 0.5,
    ) -> Dict[str, Publication]:
        """
        Checks if the ttl of each key is above 1/2 of the max ttl,
        We allow one miss since drain could delay the refresh
        """

        area_to_invalid_keyvals = {}
        for area, publication in publications.items():
            invalid_keyvals = {
                k: v
                for (k, v) in publication.keyVals.items()
                # - ttl can be INFINITY(represented by -1). Skip alerting;
                if (v.ttl >= 0 and v.ttl < threshold * ttl)
                or (v.ttl < 0 and v.ttl != Consts.CONST_TTL_INF)
            }

            # Map the invalid k-v pairs to a publication for displaying purpose
            if len(invalid_keyvals) > 0:
                area_to_invalid_keyvals[area] = Publication(keyVals=invalid_keyvals)
        return area_to_invalid_keyvals

    def _print_key_ttl_check(
        self,
        area_to_invalid_keys: Dict[str, Publication],
        ttl: int,
        threshold: float = 0.5,
    ) -> None:
        """
        Prints result of the ttl check along with information about keys with too low ttl
        The ttl printed is the ttl of the key right before the check occured
        """
        click.echo(
            self.validation_result_str(
                "kvStore", "Key TTL Check", len(area_to_invalid_keys) == 0
            )
        )

        click.echo(
            f"Configured Time-To-Live(TTL): {ttl}ms, Threshold to alarm: {int(threshold * ttl)}ms"
        )

        if len(area_to_invalid_keys) > 0:
            click.echo("Key-Value pairs with unexpected low TTL:")
            self.print_kvstore_keys(
                area_to_invalid_keys,
                True,  # Print TTL option
                False,  # Print with JSON format option
            )

    def _print_peer_state_check(
        self,
        invalid_peers: Dict[str, Dict[str, PeerSpec]],
        host_id: str,
    ) -> None:
        """
        Prints the result of the peer state check and a list of
        peers not in INITIALIZED state if there are any
        """

        click.echo(
            self.validation_result_str(
                "kvStore", "peer state check", len(invalid_peers) == 0
            )
        )

        if len(invalid_peers) > 0:
            click.echo("Information about Peers in other states:")

            self.print_peers(host_id, invalid_peers)

    def _print_local_node_advertising_check(
        self, is_advertising_adj: bool, is_advertising_prefix: bool
    ) -> None:
        """
        Prints the result of the validation check and the which keys are not being advertised
        if the validation check fails
        """

        is_pass = is_advertising_adj and is_advertising_prefix
        click.echo(
            self.validation_result_str(
                "kvStore",
                "prefix and adjacency key advertisement check",
                is_pass,
            )
        )

        if not is_pass:
            click.echo(
                f"Adjacency key originaton: {self.pass_fail_str(is_advertising_adj)}"
            )
            click.echo(
                f"Prefix key origination: {self.pass_fail_str(is_advertising_prefix)}"
            )
