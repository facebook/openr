#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import asyncio
import datetime
import hashlib
import json
import re
import string
import sys
import time
from builtins import str
from collections import defaultdict
from itertools import combinations
from typing import Any, Callable, Dict, List, Optional, Set, Union

import bunch
import hexdump
import openr.thrift.KvStore.types as _openr_thrift_KvStore_types
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.clients.openr_client import get_openr_ctrl_client, get_openr_ctrl_cpp_client
from openr.KvStore import ttypes as kv_store_types
from openr.Lsdb import ttypes as lsdb_types
from openr.Network import ttypes as network_types
from openr.OpenrCtrl import OpenrCtrl
from openr.thrift.OpenrCtrlCpp.clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing, serializer
from openr.utils.consts import Consts
from thrift.py3.client import ClientType


class KvStoreCmdBase(OpenrCtrlCmd):
    def __init__(self, cli_opts: bunch.Bunch):
        super().__init__(cli_opts)

    def _init_area(self, client: Union[OpenrCtrl.Client, OpenrCtrlCppClient]):
        # find out if area feature is supported
        # TODO: remove self.area_feature as it will be supported by default
        self.area_feature = True

        # get list of areas if area feature is supported.
        self.areas = set()
        if self.area_feature:
            self.areas = utils.get_areas_list(client)
            if self.cli_opts.area != "":
                if self.cli_opts.area in self.areas:
                    self.areas = {self.cli_opts.area}
                else:
                    print(f"Invalid area specified: {self.cli_opts.area}")
                    print(f"Valid areas: {self.areas}")
                    sys.exit(1)

    # @override
    def run(self, *args, **kwargs) -> None:
        """
        run method that invokes _run with client and arguments
        """

        with get_openr_ctrl_client(self.host, self.cli_opts) as client:
            self._init_area(client)
            self._run(client, *args, **kwargs)

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
        @param: publication - kv_store_types.Publication: the publication for parsing
        @param: nodes - set: the set of nodes for parsing
        @param: parse_func - function: the parsing function
        """

        for (key, value) in sorted(publication.keyVals.items(), key=lambda x: x[0]):
            reported_node_name = key.split(":")[1]
            if "all" not in nodes and reported_node_name not in nodes:
                continue

            parse_func(container, value)

    def get_node_to_ips(self, client: OpenrCtrl.Client, area: str = None) -> Dict:
        """ get the dict of all nodes to their IP in the network """

        node_dict = {}
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        resp = kv_store_types.Publication()
        if not self.area_feature:
            resp = client.getKvStoreKeyValsFiltered(keyDumpParams)
        else:
            if area is None:
                print(f"Error: Must specify one of the areas: {self.areas}")
                sys.exit(1)
            resp = client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)

        prefix_maps = utils.collate_prefix_keys(resp.keyVals)
        for node, prefix_db in prefix_maps.items():
            node_dict[node] = self.get_node_ip(prefix_db)

        return node_dict

    def get_node_ip(self, prefix_db: lsdb_types.PrefixDatabase) -> Any:
        """get routable IP address of node from it's prefix database"""

        # First look for LOOPBACK prefix
        for prefix_entry in prefix_db.prefixEntries:
            if prefix_entry.type == network_types.PrefixType.LOOPBACK:
                return ipnetwork.sprint_addr(prefix_entry.prefix.prefixAddress.addr)

        # Next look for PREFIX_ALLOCATOR prefix if any
        for prefix_entry in prefix_db.prefixEntries:
            if prefix_entry.type == network_types.PrefixType.PREFIX_ALLOCATOR:
                return utils.alloc_prefix_to_loopback_ip_str(prefix_entry.prefix)

        # Else return None
        return None

    def get_area_id(self) -> str:
        if not self.area_feature:
            return None
        if 1 != len(self.areas):
            print(f"Error: Must specify one of the areas: {self.areas}")
            sys.exit(1)
        (area,) = self.areas
        return area


class KvPrefixesCmd(KvStoreCmdBase):
    def _run(
        self,
        client: OpenrCtrl.Client,
        nodes: set,
        json: bool,
        prefix: str,
        client_type: str,
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        resp = client.getKvStoreKeyValsFiltered(keyDumpParams)
        self.print_prefix({None: resp}, nodes, json, prefix, client_type)

    def print_prefix(
        self,
        resp: Dict[str, kv_store_types.Publication],
        nodes: set,
        json: bool,
        prefix: str,
        client_type: str,
    ):
        all_kv = kv_store_types.Publication()
        all_kv.keyVals = {}
        for _, val in resp.items():
            all_kv.keyVals.update(val.keyVals)
        if json:
            utils.print_prefixes_json(
                all_kv, nodes, prefix, client_type, self.iter_publication
            )
        else:
            utils.print_prefixes_table(
                all_kv, nodes, prefix, client_type, self.iter_publication
            )


class PrefixesCmd(KvPrefixesCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        nodes: set,
        json: bool,
        prefix: str = "",
        client_type: str = "",
    ) -> None:
        if not self.area_feature:
            super()._run(client, nodes, json, prefix, client_type)
            return
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        area_kv = {}
        for area in self.areas:
            resp = client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)
            area_kv[area] = resp
        self.print_prefix(area_kv, nodes, json, prefix, client_type)


class KvKeysCmd(KvStoreCmdBase):
    def _run(
        self,
        client: OpenrCtrl.Client,
        json: bool,
        prefix: Any,
        originator: Any = None,
        ttl: bool = False,
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(
            prefix, [originator] if originator else None
        )
        resp = client.getKvStoreKeyValsFiltered(keyDumpParams)
        self.print_kvstore_keys({None: resp}, ttl, json)

    def print_kvstore_keys(
        self, resp: Dict[str, kv_store_types.Publication], ttl: bool, json: bool
    ) -> None:
        """ print keys from raw publication from KvStore"""

        # Export in json format if enabled
        if json:
            all_kv = {}
            for _, kv in resp.items():
                all_kv.update(kv.keyVals)

            # Force set value to None
            for value in all_kv.values():
                value.value = None

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
                kv_size = 32 + len(key) + len(value.originatorId) + len(value.value)
                db_bytes += kv_size

                hash_offset = "+" if value.hash > 0 else ""
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


class KeysCmd(KvKeysCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        json: bool,
        prefix: Any,
        originator: Any = None,
        ttl: bool = False,
    ) -> None:
        if not self.area_feature:
            super()._run(client, json, prefix, originator, ttl)
            return

        keyDumpParams = self.buildKvStoreKeyDumpParams(
            prefix, [originator] if originator else None
        )

        area_kv = {}
        for area in self.areas:
            resp = client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)
            area_kv[area] = resp

        self.print_kvstore_keys(area_kv, ttl, json)


class KvKeyValsCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, keys: List[str]) -> None:
        resp = client.getKvStoreKeyVals(keys)
        self.print_kvstore_values(resp)

    def deserialize_kvstore_publication(self, key, value):
        """ classify kvstore prefix and return the corresponding deserialized obj """

        options = {
            Consts.PREFIX_DB_MARKER: lsdb_types.PrefixDatabase,
            Consts.ADJ_DB_MARKER: lsdb_types.AdjacencyDatabase,
        }

        prefix_type = key.split(":")[0] + ":"
        if prefix_type in options.keys():
            return serializer.deserialize_thrift_object(
                value.value, options[prefix_type]
            )
        else:
            return None

    def print_kvstore_values(
        self, resp: kv_store_types.Publication, area: str = None
    ) -> None:
        """ print values from raw publication from KvStore"""

        rows = []
        for key, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            val = self.deserialize_kvstore_publication(key, value)
            if not val:
                if all(
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


class KeyValsCmd(KvKeyValsCmd):
    def _run(self, client: OpenrCtrl.Client, keys: List[str]) -> None:
        if not self.area_feature:
            super()._run(client, keys)
            return

        for area in self.areas:
            resp = client.getKvStoreKeyValsArea(keys, area)
            if len(resp.keyVals):
                self.print_kvstore_values(resp, area)


class KvNodesCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        prefix_keys = client.getKvStoreKeyValsFiltered(
            self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        )
        adj_keys = client.getKvStoreKeyValsFiltered(
            self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
        )
        host_id = client.getMyNodeName()
        self.print_kvstore_nodes(
            self.get_connected_nodes(adj_keys, host_id), prefix_keys, host_id
        )

    def get_connected_nodes(
        self, adj_keys: kv_store_types.Publication, node_id: str
    ) -> Set[str]:
        """
        Build graph of adjacencies and return list of connected node from
        current node-id
        """
        import networkx as nx

        edges = set()
        graph = nx.Graph()
        for adj_value in adj_keys.keyVals.values():
            adj_db = serializer.deserialize_thrift_object(
                adj_value.value, lsdb_types.AdjacencyDatabase
            )
            graph.add_node(adj_db.thisNodeName)
            for adj in adj_db.adjacencies:
                # Add edge only when we see the reverse side of it.
                if (adj.otherNodeName, adj_db.thisNodeName, adj.otherIfName) in edges:
                    graph.add_edge(adj.otherNodeName, adj_db.thisNodeName)
                    continue
                edges.add((adj_db.thisNodeName, adj.otherNodeName, adj.ifName))
        return nx.node_connected_component(graph, node_id)

    def print_kvstore_nodes(
        self,
        connected_nodes: Set[str],
        prefix_keys: kv_store_types.Publication,
        host_id: str,
        node_area: Dict[str, str] = None,
    ) -> None:
        """
        Print kvstore nodes information. Their loopback and reachability
        information.
        """

        def _parse_loopback_addrs(addrs, value):
            v4_addrs = addrs["v4"]
            v6_addrs = addrs["v6"]
            prefix_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.PrefixDatabase
            )

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


class NodesCmd(KvNodesCmd):
    def _run(self, client: OpenrCtrl.Client) -> None:
        if not self.area_feature:
            super()._run(client)
            return

        all_kv = kv_store_types.Publication()
        all_kv.keyVals = {}
        node_area = {}
        nodes = set()
        for area in self.areas:
            prefix_keys = client.getKvStoreKeyValsFilteredArea(
                self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER), area
            )
            all_kv.keyVals.update(prefix_keys.keyVals)
            adj_keys = client.getKvStoreKeyValsFilteredArea(
                self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER), area
            )
            host_id = client.getMyNodeName()
            node_set = self.get_connected_nodes(adj_keys, host_id)
            # save area associated with each node
            for node in node_set:
                node_area[node] = area
            nodes.update(node_set)

        self.print_kvstore_nodes(nodes, all_kv, host_id, node_area)


class KvAdjCmd(KvStoreCmdBase):
    def _run(
        self, client: OpenrCtrl.Client, nodes: set, bidir: bool, json: bool
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
        publication = client.getKvStoreKeyValsFiltered(keyDumpParams)
        self.print_adj({None: publication}, nodes, bidir, json)

    def print_adj(
        self, publications: Dict[str, kv_store_types.Publication], nodes, bidir, json
    ):
        adjs_list = defaultdict(list)
        # get list of adjancencies from each area and add it to the final
        # adjacency DB.
        adjs_map = {}
        for area, publication in publications.items():
            adjs = utils.adj_dbs_to_dict(
                publication, nodes, bidir, self.iter_publication
            )
            # handle case when area has no adjacencies
            if len(adjs):
                adjs_map = adjs
            else:
                continue
            for key, val in adjs_map.items():
                for adj_entry in val["adjacencies"]:
                    adj_entry["area"] = area
                adjs_list[key].extend(val["adjacencies"])

        for key, val in adjs_list.items():
            adjs_map[key]["adjacencies"] = val

        if json:
            utils.print_json(adjs_map)
        else:
            utils.print_adjs_table(adjs_map)


class AdjCmd(KvAdjCmd):
    def _run(
        self, client: OpenrCtrl.Client, nodes: set, bidir: bool, json: bool
    ) -> None:
        if not self.area_feature:
            super()._run(client, nodes, bidir, json)
            return

        publications = {}
        for area in self.areas:
            keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
            publications[area] = client.getKvStoreKeyValsFilteredArea(
                keyDumpParams, area
            )
        self.print_adj(publications, nodes, bidir, json)


class Areas(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, in_json: bool) -> None:
        if not self.area_feature:
            return

        if in_json:
            print(json.dumps(list(self.areas)))
        else:
            print(f"Areas configured: {self.areas}")


class KvFloodCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, roots: List[str]) -> None:
        spt_infos = client.getSpanningTreeInfos()
        utils.print_spt_infos(spt_infos, roots)


class FloodCmd(KvFloodCmd):
    def _run(self, client: OpenrCtrl.Client, roots: List[str]) -> None:
        if not self.area_feature:
            super()._run(client, roots)
            return

        for area in self.areas:
            spt_infos = client.getSpanningTreeInfos(area)
            utils.print_spt_infos(spt_infos, roots, area)


class KvShowAdjNodeCmd(KvStoreCmdBase):
    def _run(
        self, client: OpenrCtrl.Client, nodes: set, node: Any, interface: Any
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
        publication = client.getKvStoreKeyValsFiltered(keyDumpParams)
        self.printAdjNode(publication, nodes)

    def printAdjNode(self, publication, nodes, node, interface):
        adjs_map = utils.adj_dbs_to_dict(
            publication, nodes, True, self.iter_publication
        )
        utils.print_adjs_table(adjs_map, node, interface)


class ShowAdjNodeCmd(KvShowAdjNodeCmd):
    def _run(
        self, client: OpenrCtrl.Client, nodes: set, node: Any, interface: Any
    ) -> None:
        if not self.area_feature:
            super()._run(client, nodes, interface)
            return

        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
        resp = kv_store_types.Publication()
        resp.keyVals = {}
        for area in self.areas:
            publication = client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)
            resp.keyVals.update(publication.keyVals)
        self.printAdjNode(resp, nodes, node, interface)


class KvCompareCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, nodes: set) -> None:
        area = self.get_area_id()

        all_nodes_to_ips = self.get_node_to_ips(client, area)
        if nodes:
            nodes = set(nodes.strip().split(","))
            if "all" in nodes:
                nodes = list(all_nodes_to_ips.keys())
            host_id = client.getMyNodeName()
            if host_id in nodes:
                nodes.remove(host_id)

            keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ALL_DB_MARKER)
            pub = None
            if not self.area_feature:
                pub = client.getKvStoreKeyValsFiltered(keyDumpParams)
            else:
                pub = client.getKvStoreKeyValsFilteredArea(keyDumpParams, area)
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips, area)
            for node in kv_dict:
                self.compare(pub.keyVals, kv_dict[node], host_id, node)
        else:
            nodes = list(all_nodes_to_ips.keys())
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips, area)
            for our_node, other_node in combinations(kv_dict.keys(), 2):
                self.compare(
                    kv_dict[our_node], kv_dict[other_node], our_node, other_node
                )

    def compare(self, our_kvs, other_kvs, our_node, other_node):
        """ print kv delta """

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
        """ print db delta """

        if key.startswith(Consts.PREFIX_DB_MARKER):
            prefix_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.PrefixDatabase
            )
            other_prefix_db = serializer.deserialize_thrift_object(
                other_val.value, lsdb_types.PrefixDatabase
            )
            other_prefix_set = {}
            utils.update_global_prefix_db(other_prefix_set, other_prefix_db)
            lines = utils.sprint_prefixes_db_delta(other_prefix_set, prefix_db)

        elif key.startswith(Consts.ADJ_DB_MARKER):
            adj_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.AdjacencyDatabase
            )
            other_adj_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.AdjacencyDatabase
            )
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
        """ print key delta """

        print(
            printing.render_vertical_table(
                [["key: {} only in {} kv store".format(key, node)]]
            )
        )

    def dump_nodes_kvs(self, nodes: set, all_nodes_to_ips: Dict, area: str = None):
        """ get the kvs of a set of nodes """

        kv_dict = {}
        for node in nodes:
            node_ip = all_nodes_to_ips.get(node, node)
            kv = utils.dump_node_kvs(self.cli_opts, node_ip, area)
            if kv is not None:
                kv_dict[node] = kv.keyVals
                print("dumped kv from {}".format(node))
        return kv_dict


class KvPeersCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        peers = client.getKvStorePeers()
        self.print_peers(client, {None: peers})

    def print_peers(self, client: OpenrCtrl.Client, peers_list: Dict[str, Any]) -> None:
        """ print the Kv Store peers """

        host_id = client.getMyNodeName()
        caption = "{}'s peers".format(host_id)

        rows = []
        for area, peers in peers_list.items():
            area = area if area is not None else "N/A"
            for (key, value) in sorted(peers.items(), key=lambda x: x[0]):
                row = [f"{key}, area:{area}"]
                row.append("cmd via {}".format(value.cmdUrl))
                rows.append(row)

        print(printing.render_vertical_table(rows, caption=caption))


class PeersCmd(KvPeersCmd):
    def _run(self, client: OpenrCtrl.Client) -> None:
        if not self.area_feature:
            super()._run(client)
            return
        peers_list = {}
        for area in self.areas:
            peers_list[area] = client.getKvStorePeersArea(area)
        self.print_peers(client, peers_list)


class EraseKeyCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, key: str) -> None:
        area = self.get_area_id()
        publication = None
        if area is None:
            publication = client.getKvStoreKeyVals([key])
        else:
            publication = client.getKvStoreKeyValsArea([key], area)
        keyVals = publication.keyVals

        if key not in keyVals:
            print("Error: Key {} not found in KvStore.".format(key))
            sys.exit(1)

        # Get and modify the key
        val = keyVals.get(key)
        val.value = None
        val.ttl = 256  # set new ttl to 256ms (its decremented 1ms on every hop)
        val.ttlVersion += 1  # bump up ttl version

        print(keyVals)

        if area is None:
            client.setKvStoreKeyVals(kv_store_types.KeySetParams(keyVals))
        else:
            client.setKvStoreKeyVals(kv_store_types.KeySetParams(keyVals), area)

        print("Success: key {} will be erased soon from all KvStores.".format(key))


class SetKeyCmd(KvStoreCmdBase):
    def _run(
        self,
        client: OpenrCtrl.Client,
        key: str,
        value: Any,
        originator: str,
        version: Any,
        ttl: int,
    ) -> None:
        area = self.get_area_id()
        val = kv_store_types.Value()

        if version is None:
            # Retrieve existing Value from KvStore
            publication = None
            if area is None:
                publication = client.getKvStoreKeyVals([key])
            else:
                publication = client.getKvStoreKeyValsArea([key], area)
            if key in publication.keyVals:
                existing_val = publication.keyVals.get(key)
                print(
                    "Key {} found in KvStore w/ version {}. Overwriting with"
                    " higher version ...".format(key, existing_val.version)
                )
                version = existing_val.version + 1
            else:
                version = 1
        val.version = version

        val.originatorId = originator
        val.value = value
        val.ttl = ttl
        val.ttlVersion = 1

        # Advertise publication back to KvStore
        keyVals = {key: val}
        if area is None:
            client.setKvStoreKeyVals(kv_store_types.KeySetParams(keyVals))
        else:
            client.setKvStoreKeyVals(kv_store_types.KeySetParams(keyVals), area)
        print(
            "Success: Set key {} with version {} and ttl {} successfully"
            " in KvStore. This does not guarantee that value is updated"
            " in KvStore as old value can be persisted back".format(
                key,
                val.version,
                val.ttl if val.ttl != Consts.CONST_TTL_INF else "infinity",
            )
        )


class KvSignatureCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, prefix: str) -> None:
        area = self.get_area_id()
        keyDumpParams = self.buildKvStoreKeyDumpParams(prefix)
        resp = None
        if area is None:
            resp = client.getKvStoreHashFiltered(keyDumpParams)
        else:
            resp = client.getKvStoreHashFilteredArea(keyDumpParams, area)

        signature = hashlib.sha256()
        for _, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            signature.update(str(value.hash).encode("utf-8"))

        print("sha256: {}".format(signature.hexdigest()))


class SnoopCmd(KvStoreCmdBase):

    # @override
    def run(self, *args, **kwargs) -> None:
        """
        Override run method to create py3 client for streaming.
        """

        async def _wrapper():
            client_type = ClientType.THRIFT_ROCKET_CLIENT_TYPE
            async with get_openr_ctrl_cpp_client(
                self.host, self.cli_opts, client_type=client_type
            ) as client:
                # NOTE: No area initialized
                await self._run(client, *args, **kwargs)

        loop = asyncio.get_event_loop()
        loop.run_until_complete(_wrapper())
        loop.close()

    async def _run(
        self,
        client: OpenrCtrlCppClient,
        delta: bool,
        ttl: bool,
        regexes: Optional[List[str]],
        duration: int,
        originator_ids: Optional[List[str]],
        match_all: bool = True,
    ) -> None:
        # TODO: Fix area specifier for snoop. Intentionally setting to None to
        # snoop across all area once. It will be easier when we migrate all the
        # APIs to async
        # area = await self.get_area_id()
        area = None
        kvDumpParams = _openr_thrift_KvStore_types.KeyDumpParams(
            ignoreTtl=not ttl,
            keys=regexes,
            originatorIds=originator_ids,
            oper=_openr_thrift_KvStore_types.FilterOperator.AND
            if match_all
            else _openr_thrift_KvStore_types.FilterOperator.OR,
        )

        print("Retrieving and subcribing KvStore ... ")
        snapshot, updates = await client.subscribeAndGetKvStoreFiltered(kvDumpParams)
        global_dbs = self.process_snapshot(snapshot, area)
        self.print_delta(snapshot, ttl, delta, global_dbs)
        print("Magic begins here ... \n")

        start_time = time.time()
        awaited_updates = None
        while True:
            # Break if it is time
            if duration > 0 and time.time() - start_time > duration:
                break

            # Await for an update
            if not awaited_updates:
                awaited_updates = [updates.__anext__()]
            done, awaited_updates = await asyncio.wait(awaited_updates, timeout=1)
            if not done:
                continue
            else:
                msg = await done.pop()

            # filter out messages for area if specified
            if area is None or msg.area == area:
                self.print_expired_keys(msg, global_dbs)
                self.print_delta(msg, ttl, delta, global_dbs)

    def print_expired_keys(self, msg: kv_store_types.Publication, global_dbs: Dict):
        rows = []
        if len(msg.expiredKeys):
            print("Traversal List: {}".format(msg.nodeIds))

        for key in msg.expiredKeys:
            rows.append(["Key: {} got expired".format(key)])

            # Delete key from global DBs
            global_dbs.publications.pop(key, None)
            if key.startswith(Consts.ADJ_DB_MARKER):
                global_dbs.adjs.pop(key.split(":")[1], None)

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
                    node_prefix_set = global_dbs.prefixes[prefix_match.group("node")]
                    node_prefix_set = node_prefix_set - prefix_set
                else:
                    global_dbs.prefixes.pop(key.split(":")[1], None)
        if rows:
            print(printing.render_vertical_table(rows, timestamp=True))

    def print_delta(
        self, msg: kv_store_types.Publication, ttl: bool, delta: bool, global_dbs: Dict
    ):

        for key, value in msg.keyVals.items():
            if value.value is None:
                print("Traversal List: {}".format(msg.nodeIds))
                self.print_publication_delta(
                    "Key: {}, ttl update".format(key),
                    "ttl: {}, ttlVersion: {}".format(value.ttl, value.ttlVersion),
                    timestamp=True,
                )
                continue

            if key.startswith(Consts.ADJ_DB_MARKER):
                self.print_adj_delta(
                    key, value, delta, global_dbs.adjs, global_dbs.publications
                )
                continue

            if key.startswith(Consts.PREFIX_DB_MARKER):
                self.print_prefix_delta(
                    key, value, delta, global_dbs.prefixes, global_dbs.publications
                )
                continue

            print("Traversal List: {}".format(msg.nodeIds))
            self.print_publication_delta(
                "Key: {} update".format(key),
                utils.sprint_pub_update(global_dbs.publications, key, value),
                timestamp=True,
            )

    def print_prefix_delta(
        self,
        key: str,
        value: kv_store_types.Publication,
        delta: bool,
        global_prefix_db: Dict,
        global_publication_db: Dict,
    ):
        prefix_db = serializer.deserialize_thrift_object(
            value.value, lsdb_types.PrefixDatabase
        )
        if delta:
            lines = "\n".join(
                utils.sprint_prefixes_db_delta(global_prefix_db, prefix_db, key)
            )
        else:
            lines = utils.sprint_prefixes_db_full(prefix_db)

        if lines:
            self.print_publication_delta(
                "{}'s prefixes".format(prefix_db.thisNodeName),
                utils.sprint_pub_update(global_publication_db, key, value),
                lines,
                timestamp=True,
            )

        utils.update_global_prefix_db(global_prefix_db, prefix_db, key)

    def print_adj_delta(
        self,
        key: str,
        value: kv_store_types.Value,
        delta: bool,
        global_adj_db: Dict,
        global_publication_db: Dict,
    ):
        new_adj_db = serializer.deserialize_thrift_object(
            value.value, lsdb_types.AdjacencyDatabase
        )
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
                utils.sprint_pub_update(global_publication_db, key, value),
                lines,
                timestamp=True,
            )

        utils.update_global_adj_db(global_adj_db, new_adj_db)

    def process_snapshot(self, resp: kv_store_types.Publication, area: str) -> Dict:
        global_dbs = bunch.Bunch(
            {
                "prefixes": {},
                "adjs": {},
                "publications": {},  # map(key -> kv_store_types.Value)
            }
        )

        # Filter key-vals based for an area if specified
        if area:
            resp.keyVals = {k: v for k, v in resp.keyVals.items() if v.area == area}

        # Populate global_dbs
        global_dbs.prefixes = utils.build_global_prefix_db(resp)
        global_dbs.adjs = utils.build_global_adj_db(resp)
        for key, value in resp.keyVals.items():
            global_dbs.publications[key] = value

        print("Done. Loaded {} initial key-values".format(len(resp.keyVals)))
        return global_dbs


class KvAllocationsListCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY
        resp = client.getKvStoreKeyVals([key])
        self.print_allocations(key, resp.keyVals)

    def print_allocations(
        self, key: str, keyVals: kv_store_types.KeyVals, area: str = None
    ) -> None:
        if key not in keyVals:
            print("Static allocation is not set in KvStore")
        else:
            area_str = (
                "" if area is None else f'Static prefix allocations in area "{area}"'
            )
            print(area_str)
            utils.print_allocations_table(keyVals.get(key).value)


class AllocationsListCmd(KvAllocationsListCmd):
    def _run(self, client: OpenrCtrl.Client) -> None:
        if not self.area_feature:
            super()._run(client)
            return

        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY
        for area in self.areas:
            resp = client.getKvStoreKeyValsArea([key], area)
            self.print_allocations(key, resp.keyVals, area)


class AllocationsSetCmd(SetKeyCmd):
    def _run(self, client: OpenrCtrl.Client, node_name: str, prefix_str: str) -> None:
        area = self.get_area_id()
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY

        # Retrieve previous allocation
        resp = None
        if area is None:
            resp = client.getKvStoreKeyVals([key])
        else:
            resp = client.getKvStoreKeyValsArea([key], area)
        allocs = None
        if key in resp.keyVals:
            allocs = serializer.deserialize_thrift_object(
                resp.keyVals.get(key).value, openr_types.StaticAllocation
            )
        else:
            allocs = openr_types.StaticAllocation(nodePrefixes={})

        # Return if there is no change
        prefix = ipnetwork.ip_str_to_prefix(prefix_str)
        if allocs.nodePrefixes.get(node_name) == prefix:
            print(
                "No changes needed. {}'s prefix is already set to {}".format(
                    node_name, prefix_str
                )
            )
            return

        # Update value in KvStore
        allocs.nodePrefixes[node_name] = prefix
        value = serializer.serialize_thrift_object(allocs)

        super(AllocationsSetCmd, self)._run(
            client, key, value, "breeze", None, Consts.CONST_TTL_INF
        )


class AllocationsUnsetCmd(SetKeyCmd):
    def _run(self, client: OpenrCtrl.Client, node_name: str) -> None:
        area = self.get_area_id()
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY

        # Retrieve previous allocation
        resp = None
        if area is None:
            resp = client.getKvStoreKeyVals([key])
        else:
            resp = client.getKvStoreKeyValsArea([key], area)
        allocs = None
        if key in resp.keyVals:
            allocs = serializer.deserialize_thrift_object(
                resp.keyVals.get(key).value, openr_types.StaticAllocation
            )
        else:
            allocs = openr_types.StaticAllocation(nodePrefixes={node_name: ""})

        # Return if there need no change
        if node_name not in allocs.nodePrefixes:
            print("No changes needed. {}'s prefix is not set".format(node_name))
            return

        # Update value in KvStore
        del allocs.nodePrefixes[node_name]
        value = serializer.serialize_thrift_object(allocs)

        super(AllocationsUnsetCmd, self)._run(
            client, key, value, "breeze", None, Consts.CONST_TTL_INF
        )
