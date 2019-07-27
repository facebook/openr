#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import datetime
import hashlib
import re
import string
import sys
import time
from builtins import str
from itertools import combinations
from typing import Any, Callable, Dict, List

import bunch
import hexdump
import zmq
from openr.AllocPrefix import ttypes as alloc_types
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.clients import kvstore_subscriber
from openr.KvStore import ttypes as kv_store_types
from openr.Lsdb import ttypes as lsdb_types
from openr.Network import ttypes as network_types
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import ipnetwork, printing, serializer
from openr.utils.consts import Consts


class KvStoreCmdBase(OpenrCtrlCmd):
    def print_publication_delta(
        self, title: str, pub_update: List[str], sprint_db: str = ""
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
                ]
            )
        )

    def print_timestamp(self) -> None:
        print(
            "Timestamp: {}".format(
                datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
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

    def get_node_to_ips(self, client: OpenrCtrl.Client) -> Dict:
        """ get the dict of all nodes to their IP in the network """

        node_dict = {}
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        resp = client.getKvStoreKeyValsFiltered(keyDumpParams)
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


class PrefixesCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, nodes: Any, json: bool) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        resp = client.getKvStoreKeyValsFiltered(keyDumpParams)
        if json:
            utils.print_prefixes_json(resp, nodes, self.iter_publication)
        else:
            utils.print_prefixes_table(resp, nodes, self.iter_publication)


class KeysCmd(KvStoreCmdBase):
    def _run(
        self,
        client: OpenrCtrl.Client,
        json: bool,
        prefix: Any,
        originator: Any = None,
        ttl: bool = False,
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(prefix)
        resp = client.getKvStoreKeyValsFiltered(keyDumpParams)
        self.print_kvstore_keys(resp, ttl, json)

    def print_kvstore_keys(
        self, resp: kv_store_types.Publication, ttl: bool, json: bool
    ) -> None:
        """ print keys from raw publication from KvStore"""

        # Force set value to None
        for value in resp.keyVals.values():
            value.value = None

        # Export in json format if enabled
        if json:
            data = {}
            for k, v in resp.keyVals.items():
                data[k] = utils.thrift_to_dict(v)
            print(utils.json_dumps(data))
            return

        rows = []
        for key, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            hash_offset = "+" if value.hash > 0 else ""
            if ttl:
                if value.ttl != Consts.CONST_TTL_INF:
                    ttlStr = str(datetime.timedelta(milliseconds=value.ttl))
                else:
                    ttlStr = "Inf"
                rows.append(
                    [
                        key,
                        value.originatorId,
                        value.version,
                        "{}{:x}".format(hash_offset, value.hash),
                        "{} - {}".format(ttlStr, value.ttlVersion),
                    ]
                )
            else:
                rows.append(
                    [
                        key,
                        value.originatorId,
                        value.version,
                        "{}{:x}".format(hash_offset, value.hash),
                    ]
                )

        caption = "Available keys in KvStore"
        column_labels = ["Key", "Originator", "Ver", "Hash"]
        if ttl:
            column_labels = column_labels + ["TTL - Ver"]

        print(printing.render_horizontal_table(rows, column_labels, caption))


class KeyValsCmd(KvStoreCmdBase):
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

    def print_kvstore_values(self, resp: kv_store_types.Publication) -> None:
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

        caption = "Dump key-value pairs in KvStore"
        print(printing.render_vertical_table(rows, caption=caption))


class NodesCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.PREFIX_DB_MARKER)
        resp = client.getKvStoreKeyValsFiltered(keyDumpParams)
        host_id = utils.get_connected_node_name(self.cli_opts)
        self.print_kvstore_nodes(resp, host_id)

    def print_kvstore_nodes(self, resp, host_id):
        """ print prefixes from raw publication from KvStore

            :param resp kv_store_types.Publication: pub from kv store
        """

        def _parse_nodes(rows, value):
            prefix_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.PrefixDatabase
            )
            marker = "* " if prefix_db.thisNodeName == host_id else "> "
            row = ["{}{}".format(marker, prefix_db.thisNodeName)]
            loopback_prefixes = [
                p.prefix
                for p in prefix_db.prefixEntries
                if p.type == network_types.PrefixType.LOOPBACK
            ]
            loopback_prefixes.sort(
                key=lambda x: len(x.prefixAddress.addr), reverse=True
            )

            # pick the very first most specific prefix
            loopback_v6 = None
            loopback_v4 = None
            for p in loopback_prefixes:
                if len(p.prefixAddress.addr) == 16 and (
                    loopback_v6 is None or p.prefixLength > loopback_v6.prefixLength
                ):
                    loopback_v6 = p
                if len(p.prefixAddress.addr) == 4 and (
                    loopback_v4 is None or p.prefixLength > loopback_v4.prefixLength
                ):
                    loopback_v4 = p

            row.append(ipnetwork.sprint_prefix(loopback_v6) if loopback_v6 else "N/A")
            row.append(ipnetwork.sprint_prefix(loopback_v4) if loopback_v4 else "N/A")
            rows.append(row)

        rows = []
        self.iter_publication(rows, resp, {"all"}, _parse_nodes)

        label = ["Node", "V6-Loopback", "V4-Loopback"]

        print(printing.render_horizontal_table(rows, label))


class AdjCmd(KvStoreCmdBase):
    def _run(
        self, client: OpenrCtrl.Client, nodes: set, bidir: bool, json: bool
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
        publication = client.getKvStoreKeyValsFiltered(keyDumpParams)
        adjs_map = utils.adj_dbs_to_dict(
            publication, nodes, bidir, self.iter_publication
        )
        if json:
            utils.print_json(adjs_map)
        else:
            utils.print_adjs_table(adjs_map, self.enable_color)


class FloodCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, roots: List[str]) -> None:
        spt_infos = client.getSpanningTreeInfos()
        utils.print_spt_infos(spt_infos, roots)


class ShowAdjNodeCmd(KvStoreCmdBase):
    def _run(
        self, client: OpenrCtrl.Client, nodes: set, node: Any, interface: Any
    ) -> None:
        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
        publication = client.getKvStoreKeyValsFiltered(keyDumpParams)
        adjs_map = utils.adj_dbs_to_dict(
            publication, nodes, True, self.iter_publication
        )
        utils.print_adjs_table(adjs_map, self.enable_color, node, interface)


class KvCompareCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, nodes: set) -> None:
        all_nodes_to_ips = self.get_node_to_ips(client)
        if nodes:
            nodes = set(nodes.strip().split(","))
            if "all" in nodes:
                nodes = list(all_nodes_to_ips.keys())
            host_id = utils.get_connected_node_name(self.cli_opts)
            if host_id in nodes:
                nodes.remove(host_id)

            keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ALL_DB_MARKER)
            pub = client.getKvStoreKeyValsFiltered(keyDumpParams)
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips)
            for node in kv_dict:
                self.compare(pub.keyVals, kv_dict[node], host_id, node)

        else:
            nodes = list(all_nodes_to_ips.keys())
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips)
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

    def dump_nodes_kvs(self, nodes, all_nodes_to_ips):
        """ get the kvs of a set of nodes """

        kv_dict = {}
        for node in nodes:
            node_ip = all_nodes_to_ips.get(node, node)
            kv = utils.dump_node_kvs(self.cli_opts, node_ip)
            if kv is not None:
                kv_dict[node] = kv.keyVals
                print("dumped kv from {}".format(node))
        return kv_dict


class PeersCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        peers = client.getKvStorePeers()
        self.print_peers(peers)

    def print_peers(self, peers: kv_store_types.PeersMap) -> None:
        """ print the Kv Store peers """

        host_id = utils.get_connected_node_name(self.cli_opts)
        caption = "{}'s peers".format(host_id)

        rows = []
        for (key, value) in sorted(peers.items(), key=lambda x: x[0]):
            row = [key]
            row.append("cmd via {}".format(value.cmdUrl))
            row.append("pub via {}".format(value.pubUrl))
            rows.append(row)

        print(printing.render_vertical_table(rows, caption=caption))


class EraseKeyCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, key: str) -> None:
        publication = client.getKvStoreKeyVals([key])
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

        client.setKvStoreKeyVals(kv_store_types.KeySetParams(keyVals))

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
        val = kv_store_types.Value()

        if version is None:
            # Retrieve existing Value from KvStore
            publication = client.getKvStoreKeyVals([key])
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
        client.setKvStoreKeyVals(kv_store_types.KeySetParams(keyVals))
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
        keyDumpParams = self.buildKvStoreKeyDumpParams(prefix)
        resp = client.getKvStoreHashFiltered(keyDumpParams)

        signature = hashlib.sha256()
        for _, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            signature.update(str(value.hash).encode("utf-8"))

        print("sha256: {}".format(signature.hexdigest()))


class TopologyCmd(KvStoreCmdBase):
    def _run(
        self,
        client: OpenrCtrl.Client,
        node: str,
        bidir: bool,
        output_file: str,
        edge_label: Any,
        json: bool,
    ) -> None:

        try:
            import matplotlib.pyplot as plt
            import networkx as nx
        except ImportError:
            print(
                "Drawing topology requires `matplotlib` and `networkx` "
                "libraries. You can install them with following command and "
                "retry. \n"
                "  pip install matplotlib\n"
                "  pip install networkx"
            )
            sys.exit(1)

        rem_str = {".facebook.com": "", ".tfbnw.net": ""}
        rem_str = dict((re.escape(k), v) for k, v in rem_str.items())
        rem_pattern = re.compile("|".join(rem_str.keys()))

        keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ADJ_DB_MARKER)
        publication = client.getKvStoreKeyValsFiltered(keyDumpParams)
        nodes = list(self.get_node_to_ips(client).keys()) if not node else [node]
        adjs_map = utils.adj_dbs_to_dict(
            publication, nodes, bidir, self.iter_publication
        )

        if json:
            return self.topology_json_dump(adjs_map.items())

        G = nx.Graph()
        adj_metric_map = {}
        node_overloaded = {}

        for this_node_name, db in adjs_map.items():
            node_overloaded[
                rem_pattern.sub(
                    lambda m: rem_str[re.escape(m.group(0))], this_node_name
                )
            ] = db["overloaded"]
            for adj in db["adjacencies"]:
                adj_metric_map[(this_node_name, adj["ifName"])] = adj["metric"]

        for this_node_name, db in adjs_map.items():
            for adj in db["adjacencies"]:
                adj["color"] = "r" if adj["isOverloaded"] else "b"
                adj["adjOtherIfMetric"] = adj_metric_map[
                    (adj["otherNodeName"], adj["otherIfName"])
                ]
                G.add_edge(
                    rem_pattern.sub(
                        lambda m: rem_str[re.escape(m.group(0))], this_node_name
                    ),
                    rem_pattern.sub(
                        lambda m: rem_str[re.escape(m.group(0))], adj["otherNodeName"]
                    ),
                    **adj,
                )

        # hack to get nice fabric
        # XXX: FB Specific
        pos = {}
        eswx = 0
        sswx = 0
        fswx = 0
        rswx = 0
        blue_nodes = []
        red_nodes = []
        for node in G.nodes():
            if node_overloaded[node]:
                red_nodes.append(node)
            else:
                blue_nodes.append(node)

            if "esw" in node:
                pos[node] = [eswx, 3]
                eswx += 10
            elif "ssw" in node:
                pos[node] = [sswx, 2]
                sswx += 10
            elif "fsw" in node:
                pos[node] = [fswx, 1]
                fswx += 10
            elif "rsw" in node:
                pos[node] = [rswx, 0]
                rswx += 10

        maxswx = max(eswx, sswx, fswx, rswx)
        if maxswx > 0:
            # aesthetically pleasing multiplier (empirically determined)
            plt.figure(figsize=(maxswx * 0.5, 8))
        else:
            plt.figure(
                figsize=(min(len(G.nodes()) * 2, 150), min(len(G.nodes()) * 2, 150))
            )
            pos = nx.spring_layout(G)
        plt.axis("off")

        edge_colors = []
        for _, _, d in G.edges(data=True):
            edge_colors.append(d["color"])

        nx.draw_networkx_nodes(
            G, pos, ax=None, alpha=0.5, node_color="b", nodelist=blue_nodes
        )
        nx.draw_networkx_nodes(
            G, pos, ax=None, alpha=0.5, node_color="r", nodelist=red_nodes
        )
        nx.draw_networkx_labels(G, pos, ax=None, alpha=0.5, font_size=8)
        nx.draw_networkx_edges(
            G, pos, ax=None, alpha=0.5, font_size=8, edge_color=edge_colors
        )
        edge_labels = {}
        if node:
            if edge_label:
                edge_labels = {
                    (u, v): "<"
                    + str(d["otherIfName"])
                    + ",  "
                    + str(d["metric"])
                    + " >     <"
                    + str(d["ifName"])
                    + ", "
                    + str(d["adjOtherIfMetric"])
                    + ">"
                    for u, v, d in G.edges(data=True)
                }
            nx.draw_networkx_edge_labels(G, pos, edge_labels=edge_labels, font_size=6)

        print("Saving topology to file => {}".format(output_file))
        plt.savefig(output_file)

    def topology_json_dump(self, adjs_map_items):
        adj_topo = {}
        for this_node_name, db in adjs_map_items:
            adj_topo[this_node_name] = db
        print(utils.json_dumps(adj_topo))


class SnoopCmd(KvStoreCmdBase):
    def _run(
        self,
        client: OpenrCtrl.Client,
        delta: bool,
        ttl: bool,
        regex: str,
        duration: int,
    ) -> None:
        global_dbs = self.get_snapshot(client, delta)
        pattern = re.compile(regex)

        print("Subscribing to KvStore updates. Magic begins here ... \n")
        pub_client = kvstore_subscriber.KvStoreSubscriber(
            zmq.Context(),
            "tcp://[{}]:{}".format(self.host, self.kv_pub_port),
            timeout=1000,
        )

        start_time = time.time()
        while True:
            # End loop if it is time!
            if duration > 0 and time.time() - start_time > duration:
                break

            # we do not want to timeout. keep listening for a change
            try:
                msg = pub_client.listen()
                self.print_expired_keys(msg, regex, pattern, global_dbs)
                self.print_delta(msg, regex, pattern, ttl, delta, global_dbs)
            except zmq.error.Again:
                pass

    def print_expired_keys(self, msg, regex, pattern, global_dbs):
        rows = []
        if len(msg.expiredKeys):
            print("Traversal List: {}".format(msg.nodeIds))

        for key in msg.expiredKeys:
            if not key.startswith(regex) and not pattern.match(key):
                continue
            rows.append(["Key: {} got expired".format(key)])

            # Delete key from global DBs
            global_dbs.publications.pop(key, None)
            if key.startswith(Consts.ADJ_DB_MARKER):
                global_dbs.adjs.pop(key.split(":")[1], None)
            if key.startswith(Consts.PREFIX_DB_MARKER):
                global_dbs.prefixes.pop(key.split(":")[1], None)

        if rows:
            self.print_timestamp()
            print(printing.render_vertical_table(rows))

    def print_delta(self, msg, regex, pattern, ttl, delta, global_dbs):

        for key, value in msg.keyVals.items():
            if not key.startswith(regex) and not pattern.match(key):
                continue
            if value.value is None:
                if ttl:
                    self.print_timestamp()
                    print("Traversal List: {}".format(msg.nodeIds))
                    self.print_publication_delta(
                        "Key: {}, ttl update".format(key),
                        "ttl: {}, ttlVersion: {}".format(value.ttl, value.ttlVersion),
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

            self.print_timestamp()
            print("Traversal List: {}".format(msg.nodeIds))
            self.print_publication_delta(
                "Key: {} update".format(key),
                utils.sprint_pub_update(global_dbs.publications, key, value),
            )

    def print_prefix_delta(
        self, key, value, delta, global_prefix_db, global_publication_db
    ):
        _, reported_node_name = key.split(":", 1)
        prefix_db = serializer.deserialize_thrift_object(
            value.value, lsdb_types.PrefixDatabase
        )
        if delta:
            lines = "\n".join(
                utils.sprint_prefixes_db_delta(global_prefix_db, prefix_db)
            )
        else:
            lines = utils.sprint_prefixes_db_full(prefix_db)

        if lines:
            self.print_timestamp()
            self.print_publication_delta(
                "{}'s prefixes".format(reported_node_name),
                utils.sprint_pub_update(global_publication_db, key, value),
                lines,
            )

        utils.update_global_prefix_db(global_prefix_db, prefix_db)

    def print_adj_delta(self, key, value, delta, global_adj_db, global_publication_db):
        _, reported_node_name = key.split(":", 1)
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
            self.print_timestamp()
            self.print_publication_delta(
                "{}'s adjacencies".format(reported_node_name),
                utils.sprint_pub_update(global_publication_db, key, value),
                lines,
            )

        utils.update_global_adj_db(global_adj_db, new_adj_db)

    def get_snapshot(self, client: OpenrCtrl.Client, delta: Any) -> Dict:
        # get the active network snapshot first, so we can compute deltas
        global_dbs = bunch.Bunch(
            {
                "prefixes": {},
                "adjs": {},
                "publications": {},  # map(key -> (version, originatorId))
            }
        )

        if delta:
            print("Retrieving KvStore snapshot ... ")
            keyDumpParams = self.buildKvStoreKeyDumpParams(Consts.ALL_DB_MARKER)
            resp = client.getKvStoreKeyValsFiltered(keyDumpParams)

            global_dbs.prefixes = utils.build_global_prefix_db(resp)
            global_dbs.adjs = utils.build_global_adj_db(resp)
            for key, value in resp.keyVals.items():
                global_dbs.publications[key] = (value.version, value.originatorId)
            print("Done. Loaded {} initial key-values".format(len(resp.keyVals)))
        else:
            print("Skipping retrieval of KvStore snapshot")
        return global_dbs


class AllocationsListCmd(KvStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY
        resp = client.getKvStoreKeyVals([key])
        if key not in resp.keyVals:
            print("Static allocation is not set in KvStore")
        else:
            utils.print_allocations_table(resp.keyVals.get(key).value)


class AllocationsSetCmd(SetKeyCmd):
    def _run(self, client: OpenrCtrl.Client, node_name: str, prefix_str: str) -> None:
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY

        # Retrieve previous allocation
        resp = client.getKvStoreKeyVals([key])
        allocs = None
        if key in resp.keyVals:
            allocs = serializer.deserialize_thrift_object(
                resp.keyVals.get(key).value, alloc_types.StaticAllocation
            )
        else:
            allocs = alloc_types.StaticAllocation(nodePrefixes={})

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
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY

        # Retrieve previous allocation
        resp = client.getKvStoreKeyVals([key])
        allocs = None
        if key in resp.keyVals:
            allocs = serializer.deserialize_thrift_object(
                resp.keyVals.get(key).value, alloc_types.StaticAllocation
            )
        else:
            allocs = alloc_types.StaticAllocation(nodePrefixes={node_name: ""})

        # Return if there need no change
        if node_name not in allocs.nodePrefixes:
            print("No changes needed. {}'s prefix is not set".format(node_name))

        # Update value in KvStore
        del allocs.nodePrefixes[node_name]
        value = serializer.serialize_thrift_object(allocs)

        super(AllocationsUnsetCmd, self)._run(
            client, key, value, "breeze", None, Consts.CONST_TTL_INF
        )
