#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import
from __future__ import print_function
from __future__ import unicode_literals
from __future__ import division
from builtins import str
from builtins import object

import bunch
import datetime
import hashlib
import hexdump
import re
import string
import sys
import time
import zmq

from itertools import combinations
from openr.AllocPrefix import ttypes as alloc_types
from openr.clients import kvstore_client, kvstore_subscriber
from openr.cli.utils import utils
from openr.utils.consts import Consts
from openr.utils import ipnetwork, printing
from openr.utils import serializer

from openr.Lsdb import ttypes as lsdb_types
from openr.KvStore import ttypes as kv_store_types


def print_publication_delta(title, pub_update, sprint_db=""):
    print(printing.render_vertical_table([[
          "{}\n{}{}".format(title,
                            pub_update,
                            "\n\n{}".format(sprint_db) if sprint_db else "")]]))


def print_timestamp():
    print(
        'Timestamp: {}'.format(
            datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]))


class KvStoreCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Kvsotre client '''

        self.host = cli_opts.host
        self.kv_pub_port = cli_opts.kv_pub_port
        self.kv_rep_port = cli_opts.kv_rep_port
        self.lm_cmd_port = cli_opts.lm_cmd_port
        self.enable_color = cli_opts.enable_color

        self.client = kvstore_client.KvStoreClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.kv_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)

    def iter_publication(self, container, publication, nodes, parse_func):
        ''' parse dumped publication

            :container: container to store the generated data
            :publication kv_store_types.Publication: the publication for parsing
            :nodes set: the set of nodes for parsing
            :parse_func function: the parsing function
        '''

        for (key, value) in sorted(publication.keyVals.items(), key=lambda x: x[0]):
            _, reported_node_name = key.split(':', 1)
            if 'all' not in nodes and reported_node_name not in nodes:
                continue

            parse_func(container, value)

    def get_node_to_ips(self):
        ''' get the dict of all nodes to their IP in the network '''

        def _parse_nodes(node_dict, value):
            prefix_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.PrefixDatabase)
            node_dict[prefix_db.thisNodeName] = self.get_node_ip(prefix_db)

        node_dict = {}
        resp = self.client.dump_all_with_prefix(Consts.PREFIX_DB_MARKER)
        self.iter_publication(node_dict, resp, ['all'], _parse_nodes)

        return node_dict

    def get_node_ip(self, prefix_db):
        '''
        get routable IP address of node from it's prefix database
        :return: string representation of Node's IP addresss. Returns None if
                 no IP found.
        '''

        # First look for LOOPBACK prefix
        for prefix_entry in prefix_db.prefixEntries:
            if prefix_entry.type == lsdb_types.PrefixType.LOOPBACK:
                return ipnetwork.sprint_addr(prefix_entry.prefix.prefixAddress.addr)

        # Next look for PREFIX_ALLOCATOR prefix if any
        for prefix_entry in prefix_db.prefixEntries:
            if prefix_entry.type == lsdb_types.PrefixType.PREFIX_ALLOCATOR:
                return utils.alloc_prefix_to_loopback_ip_str(
                    prefix_entry.prefix)

        # Else return None
        return None


class PrefixesCmd(KvStoreCmd):
    def run(self, nodes, json):
        resp = self.client.dump_all_with_prefix(Consts.PREFIX_DB_MARKER)
        if json:
            utils.print_prefixes_json(resp, nodes, self.iter_publication)
        else:
            utils.print_prefixes_table(resp, nodes, self.iter_publication)


class KeysCmd(KvStoreCmd):
    def run(self, json_fmt, prefix, originator=None, ttl=False):
        if originator is not None:
            resp = self.client.dump_all_with_prefix(prefix, originator)
        else:
            resp = self.client.dump_key_with_prefix(prefix)
        self.print_kvstore_keys(resp, ttl, json_fmt)

    def print_kvstore_keys(self, resp, ttl, json_fmt):
        ''' print keys from raw publication from KvStore

            :param resp kv_store_types.Publication: pub from kv store
            :param ttl bool: Show ttl value and version if True
        '''

        # Force set value to None
        for value in resp.keyVals.values():
            value.value = None

        # Export in json format if enabled
        if json_fmt:
            data = {}
            for k, v in resp.keyVals.items():
                data[k] = utils.thrift_to_dict(v)
            print(utils.json_dumps(data))
            return

        rows = []
        for key, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            hash_offset = '+' if value.hash > 0 else ''
            if ttl:
                if value.ttl != Consts.CONST_TTL_INF:
                    ttlStr = str(datetime.timedelta(milliseconds=value.ttl))
                else:
                    ttlStr = "Inf"
                rows.append([
                    key,
                    value.originatorId,
                    value.version,
                    '{}{:x}'.format(hash_offset, value.hash),
                    '{} - {}'.format(ttlStr, value.ttlVersion),
                ])
            else:
                rows.append([
                    key,
                    value.originatorId,
                    value.version,
                    '{}{:x}'.format(hash_offset, value.hash),
                ])

        caption = "Available keys in KvStore"
        column_labels = ["Key", "Originator", "Ver", "Hash"]
        if ttl:
            column_labels = column_labels + ["TTL - Ver"]

        print(printing.render_horizontal_table(rows, column_labels, caption))


class KeyValsCmd(KvStoreCmd):
    def run(self, keys, json_fmt):
        resp = self.client.get_keys(keys)
        self.print_kvstore_values(resp, json_fmt)

    def deserialize_kvstore_publication(self, key, value):
        ''' classify kvstore prefix and return the corresponding deserialized obj '''

        options = {
            Consts.PREFIX_DB_MARKER: lsdb_types.PrefixDatabase,
            Consts.ADJ_DB_MARKER: lsdb_types.AdjacencyDatabase,
            Consts.INTERFACE_DB_MARKER: lsdb_types.InterfaceDatabase,
        }

        prefix_type = key.split(':')[0] + ":"
        if prefix_type in options.keys():
            return serializer.deserialize_thrift_object(
                value.value, options[prefix_type])
        else:
            return None

    def print_kvstore_values(self, resp, json_fmt):
        ''' print values from raw publication from KvStore

            :param resp kv_store_types.Publication: pub from kv store
        '''

        # Export in json format if enabled
        if json_fmt:
            data = {}
            for k, v in resp.keyVals.items():
                data[k] = utils.thrift_to_dict(v)
            print(utils.json_dumps(data))
            return

        rows = []

        for key, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            val = self.deserialize_kvstore_publication(key, value)
            if not val:
                if all(isinstance(c, str) and c in string.printable
                       for c in value.value):
                    val = value.value
                else:
                    val = hexdump.hexdump(value.value, 'return')

            ttl = 'INF' if value.ttl == Consts.CONST_TTL_INF else value.ttl
            rows.append(["key: {}\n  version: {}\n  originatorId: {}\n  "
                         "ttl: {}\n  ttlVersion: {}\n  value:\n    {}"
                         .format(key, value.version, value.originatorId, ttl,
                                 value.ttlVersion, val)])

        caption = "Dump key-value pairs in KvStore"
        print(printing.render_vertical_table(rows, caption=caption))


class NodesCmd(KvStoreCmd):
    def run(self):
        resp = self.client.dump_all_with_prefix(Consts.PREFIX_DB_MARKER)
        host_id = utils.get_connected_node_name(self.host, self.lm_cmd_port)
        self.print_kvstore_nodes(resp, host_id)

    def print_kvstore_nodes(self, resp, host_id):
        ''' print prefixes from raw publication from KvStore

            :param resp kv_store_types.Publication: pub from kv store
        '''

        def _parse_nodes(rows, value):
            prefix_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.PrefixDatabase)
            marker = '* ' if prefix_db.thisNodeName == host_id else '> '
            row = ["{}{}".format(marker, prefix_db.thisNodeName)]
            loopback_prefixes = [
                p.prefix for p in prefix_db.prefixEntries
                if p.type == lsdb_types.PrefixType.LOOPBACK
            ]
            loopback_prefixes.sort(key=lambda x: len(x.prefixAddress.addr),
                                   reverse=True)

            # pick the very first most specific prefix
            loopback_v6 = None
            loopback_v4 = None
            for p in loopback_prefixes:
                if len(p.prefixAddress.addr) == 16 and \
                   (loopback_v6 is None or p.prefixLength > loopback_v6.prefixLength):
                    loopback_v6 = p
                if len(p.prefixAddress.addr) == 4 and \
                   (loopback_v4 is None or p.prefixLength > loopback_v4.prefixLength):
                    loopback_v4 = p

            row.append(ipnetwork.sprint_prefix(loopback_v6) if loopback_v6 else 'N/A')
            row.append(ipnetwork.sprint_prefix(loopback_v4) if loopback_v4 else 'N/A')
            rows.append(row)

        rows = []
        self.iter_publication(rows, resp, set(['all']), _parse_nodes)

        label = ['Node', 'V6-Loopback', 'V4-Loopback']

        print(printing.render_horizontal_table(rows, label))


class AdjCmd(KvStoreCmd):
    def run(self, nodes, bidir, json):
        publication = self.client.dump_all_with_prefix(Consts.ADJ_DB_MARKER)
        adjs_map = utils.adj_dbs_to_dict(publication, nodes, bidir,
                                         self.iter_publication)
        if json:
            utils.print_json(adjs_map)
        else:
            utils.print_adjs_table(adjs_map, self.enable_color)


class ShowAdjNodeCmd(KvStoreCmd):
    def run(self, nodes, node, interface):
        publication = self.client.dump_all_with_prefix(Consts.ADJ_DB_MARKER)
        adjs_map = utils.adj_dbs_to_dict(publication, nodes, True,
                                         self.iter_publication)
        utils.print_adjs_table(adjs_map, self.enable_color, node, interface)


class InterfacesCmd(KvStoreCmd):
    def run(self, nodes, json, print_all):
        publication = self.client.dump_all_with_prefix(
            Consts.INTERFACE_DB_MARKER)
        intfs_map = utils.interface_dbs_to_dict(publication, nodes,
                                                self.iter_publication)
        if json:
            print(utils.json_dumps(intfs_map))
        else:
            utils.print_interfaces_table(intfs_map, print_all)


class KvCompareCmd(KvStoreCmd):
    def run(self, nodes):
        all_nodes_to_ips = self.get_node_to_ips()
        if nodes:
            nodes = set(nodes.strip().split(','))
            if 'all' in nodes:
                nodes = list(all_nodes_to_ips.keys())
            host_id = utils.get_connected_node_name(self.host, self.lm_cmd_port)
            if host_id in nodes:
                nodes.remove(host_id)

            our_kvs = self.client.dump_all_with_prefix().keyVals
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips)
            for node in kv_dict:
                self.compare(our_kvs, kv_dict[node], host_id, node)

        else:
            nodes = list(all_nodes_to_ips.keys())
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips)
            for our_node, other_node in combinations(kv_dict.keys(), 2):
                self.compare(kv_dict[our_node], kv_dict[other_node],
                             our_node, other_node)

    def compare(self, our_kvs, other_kvs, our_node, other_node):
        ''' print kv delta '''

        print(printing.caption_fmt(
              'kv-compare between {} and {}'.format(our_node, other_node)))

        # for comparing version and id info
        our_kv_pub_db = {}
        for key, value in our_kvs.items():
            our_kv_pub_db[key] = (value.version, value.originatorId)

        for key, value in sorted(our_kvs.items()):
            other_val = other_kvs.get(key, None)
            if other_val is None:
                self.print_key_delta(key, our_node)

            elif (key.startswith(Consts.PREFIX_DB_MARKER) or
                    key.startswith(Consts.ADJ_DB_MARKER) or
                    other_val.value != value.value):
                self.print_db_delta(key, our_kv_pub_db, value, other_val)

        for key, _ in sorted(other_kvs.items()):
            ourVal = our_kvs.get(key, None)
            if ourVal is None:
                self.print_key_delta(key, other_node)

    def print_db_delta(self, key, our_kv_pub_db, value, other_val):
        ''' print db delta '''

        if key.startswith(Consts.PREFIX_DB_MARKER):
            prefix_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.PrefixDatabase)
            other_prefix_db = serializer.deserialize_thrift_object(
                other_val.value, lsdb_types.PrefixDatabase)
            other_prefix_set = {}
            utils.update_global_prefix_db(other_prefix_set, other_prefix_db)
            lines = utils.sprint_prefixes_db_delta(other_prefix_set, prefix_db)

        elif key.startswith(Consts.ADJ_DB_MARKER):
            adj_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.AdjacencyDatabase)
            other_adj_db = serializer.deserialize_thrift_object(
                value.value, lsdb_types.AdjacencyDatabase)
            lines = utils.sprint_adj_db_delta(adj_db, other_adj_db)

        else:
            lines = None

        if lines != []:
            print_publication_delta(
                "Key: {} difference".format(key),
                utils.sprint_pub_update(our_kv_pub_db, key, other_val),
                "\n".join(lines) if lines else "")

    def print_key_delta(self, key, node):
        ''' print key delta '''

        print(printing.render_vertical_table([[
            "key: {} only in {} kv store".format(key, node)]]))

    def dump_nodes_kvs(self, nodes, all_nodes_to_ips):
        ''' get the kvs of a set of nodes '''

        kv_dict = {}
        for node in nodes:
            node_ip = all_nodes_to_ips.get(node, node)
            kv = utils.dump_node_kvs(node_ip, self.kv_rep_port)
            if kv is not None:
                kv_dict[node] = kv.keyVals
                print('dumped kv from {}'.format(node))
        return kv_dict


class PeersCmd(KvStoreCmd):
    def run(self):

        self.print_peers(self.client.dump_peers())

    def print_peers(self, peers_reply):
        ''' print the Kv Store peers '''

        host_id = utils.get_connected_node_name(self.host, self.lm_cmd_port)
        caption = '{}\'s peers'.format(host_id)

        rows = []
        for (key, value) in sorted(peers_reply.peers.items(), key=lambda x: x[0]):
            row = [key]
            row.append('cmd via {}'.format(value.cmdUrl))
            row.append('pub via {}'.format(value.pubUrl))
            rows.append(row)

        print(printing.render_vertical_table(rows, caption=caption))


class EraseKeyCmd(KvStoreCmd):
    def run(self, key):

        publication = self.client.get_keys([key])

        if key not in publication.keyVals:
            print("Error: Key {} not found in KvStore.".format(key))
            sys.exit(1)

        # Get and modify the key
        val = publication.keyVals.get(key)
        val.value = None
        val.ttl = 1           # set new ttl to 0
        val.ttlVersion += 1   # bump up ttl version

        print(publication.keyVals)
        response = self.client.set_key(publication.keyVals)
        if response != b'OK':
            print('Error: failed to set ttl to 0')
        else:
            print('Success: key {} will be erased soon from all KvStores.'
                  .format(key))


class SetKeyCmd(KvStoreCmd):
    def run(self, key, value, originator, version, ttl):

        val = kv_store_types.Value()
        if version is None:
            # Retrieve existing Value from KvStore
            publication = self.client.get_keys([key])
            if key in publication.keyVals:
                existing_val = publication.keyVals.get(key)
                print('Key {} found in KvStore w/ version {}. Overwriting with'
                      ' higher version ...'.format(key, existing_val.version))
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
        response = self.client.set_key(keyVals)
        if response != b'OK':
            print('Error: Failed to set key into KvStore')
        else:
            print('Success: Set key {} with version {} and ttl {} successfully'
                  ' in KvStore. This does not guarantee that value is updated'
                  ' in KvStore as old value can be persisted back'.format(
                      key,
                      val.version,
                      val.ttl if val.ttl != Consts.CONST_TTL_INF else 'infinity'))


class KvSignatureCmd(KvStoreCmd):
    def run(self, prefix):

        resp = self.client.dump_key_with_prefix(prefix)

        signature = hashlib.sha256()
        for _, value in sorted(resp.keyVals.items(), key=lambda x: x[0]):
            signature.update(str(value.hash).encode('utf-8'))

        print('sha256: {}'.format(signature.hexdigest()))


class TopologyCmd(KvStoreCmd):
    def run(self, node, bidir, output_file, edge_label, json):

        try:
            import matplotlib.pyplot as plt
            import networkx as nx
        except ImportError:
            print('Drawing topology requires `matplotlib` and `networkx` '
                  'libraries. You can install them with following command and '
                  'retry. \n'
                  '  pip install matplotlib\n'
                  '  pip install networkx')
            sys.exit(1)

        rem_str = {
            '.facebook.com': '',
            '.tfbnw.net': '',
        }
        rem_str = dict((re.escape(k), v) for k, v in rem_str.items())
        rem_pattern = re.compile("|".join(rem_str.keys()))

        publication = self.client.dump_all_with_prefix(Consts.ADJ_DB_MARKER)
        nodes = list(self.get_node_to_ips().keys()) if not node else [node]
        adjs_map = utils.adj_dbs_to_dict(publication, nodes, bidir,
                                         self.iter_publication)

        if json:
            return self.topology_json_dump(adjs_map.items())

        G = nx.Graph()
        adj_metric_map = {}
        node_overloaded = {}

        for this_node_name, db in adjs_map.items():
            node_overloaded[rem_pattern.sub(lambda m:
                            rem_str[re.escape(m.group(0))],
                            this_node_name)] = db['overloaded']
            for adj in db['adjacencies']:
                adj_metric_map[(this_node_name, adj['ifName'])] = adj['metric']

        for this_node_name, db in adjs_map.items():
            for adj in db['adjacencies']:
                adj['color'] = 'r' if adj['isOverloaded'] else 'b'
                adj['adjOtherIfMetric'] = adj_metric_map[(adj['otherNodeName'],
                                                            adj['otherIfName'])]
                G.add_edge(rem_pattern.sub(lambda m:
                                            rem_str[re.escape(m.group(0))],
                                            this_node_name),
                                            rem_pattern.sub(lambda m:
                                                            rem_str[re.escape(m.group(0))],
                                                            adj['otherNodeName']), **adj)

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
            if (node_overloaded[node]):
                red_nodes.append(node)
            else:
                blue_nodes.append(node)

            if 'esw' in node:
                pos[node] = [eswx, 3]
                eswx += 10
            elif 'ssw' in node:
                pos[node] = [sswx, 2]
                sswx += 10
            elif 'fsw' in node:
                pos[node] = [fswx, 1]
                fswx += 10
            elif 'rsw' in node:
                pos[node] = [rswx, 0]
                rswx += 10

        maxswx = max(eswx, sswx, fswx, rswx)
        if maxswx > 0:
            # aesthetically pleasing multiplier (empirically determined)
            plt.figure(figsize=(maxswx * 0.5, 8))
        else:
            plt.figure(figsize=(min(len(G.nodes()) * 2, 150),
                                min(len(G.nodes()) * 2, 150)))
            pos = nx.spring_layout(G)
        plt.axis('off')

        edge_colors = []
        for _, _, d in G.edges(data=True):
            edge_colors.append(d['color'])

        nx.draw_networkx_nodes(G, pos, ax=None, alpha=0.5,
                                node_color='b', nodelist=blue_nodes)
        nx.draw_networkx_nodes(G, pos, ax=None, alpha=0.5,
                                node_color='r', nodelist=red_nodes)
        nx.draw_networkx_labels(G, pos, ax=None, alpha=0.5, font_size=8)
        nx.draw_networkx_edges(G, pos, ax=None, alpha=0.5,
                                font_size=8, edge_color=edge_colors)
        edge_labels = {}
        if node:
            if edge_label:
                edge_labels = {(u, v): '<' + str(d['otherIfName']) + ',  ' +
                                str(d['metric']) +
                                ' >     <' + str(d['ifName']) +
                                ', ' + str(d['adjOtherIfMetric']) + '>'
                                for u, v, d in G.edges(data=True)}
            nx.draw_networkx_edge_labels(G, pos, edge_labels=edge_labels,
                                         font_size=6)

        print('Saving topology to file => {}'.format(output_file))
        plt.savefig(output_file)

    def topology_json_dump(self, adjs_map_items):
        adj_topo = {}
        for this_node_name, db in adjs_map_items:
            adj_topo[this_node_name] = db
        print(utils.json_dumps(adj_topo))


class SnoopCmd(KvStoreCmd):
    def run(self, delta, ttl, regex, duration):

        global_dbs = self.get_snapshot(delta)
        pattern = re.compile(regex)

        pub_client = kvstore_subscriber.KvStoreSubscriber(
            zmq.Context(),
            "tcp://[{}]:{}".format(self.host, self.kv_pub_port),
            timeout=1000)

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
        for key in msg.expiredKeys:
            if not key.startswith(regex) and not pattern.match(key):
                continue
            rows.append(["Key: {} got expired".format(key)])

            # Delete key from global DBs
            global_dbs.publications.pop(key, None)
            if key.startswith(Consts.ADJ_DB_MARKER):
                global_dbs.adjs.pop(key.split(':')[1], None)
            if key.startswith(Consts.PREFIX_DB_MARKER):
                global_dbs.prefixes.pop(key.split(':')[1], None)
            if key.startswith(Consts.INTERFACE_DB_MARKER):
                global_dbs.interfaces.pop(key.split(':')[1], None)

        if rows:
            print_timestamp()
            print(printing.render_vertical_table(rows))

    def print_delta(self, msg, regex, pattern, ttl, delta, global_dbs):

        for key, value in msg.keyVals.items():
            if not key.startswith(regex) and not pattern.match(key):
                continue
            if value.value is None:
                if ttl:
                    print_timestamp()
                    print_publication_delta(
                        "Key: {}, ttl update".format(key),
                        "ttl: {}, ttlVersion: {}".format(value.ttl,
                                                         value.ttlVersion))
                continue

            if key.startswith(Consts.ADJ_DB_MARKER):
                print_timestamp()
                self.print_adj_delta(key, value, delta,
                                     global_dbs.adjs,
                                     global_dbs.publications)
                continue

            if key.startswith(Consts.PREFIX_DB_MARKER):
                print_timestamp()
                self.print_prefix_delta(key, value, delta,
                                        global_dbs.prefixes,
                                        global_dbs.publications)
                continue

            if key.startswith(Consts.INTERFACE_DB_MARKER):
                print_timestamp()
                self.print_interface_delta(key, value, delta,
                                           global_dbs.interfaces,
                                           global_dbs.publications)
                continue

            if delta:
                print_timestamp()
                print_publication_delta(
                    "Key: {} update".format(key),
                    utils.sprint_pub_update(global_dbs.publications,
                                            key, value))

    def print_prefix_delta(self, key, value, delta, global_prefix_db,
                           global_publication_db):
        _, reported_node_name = key.split(':', 1)
        prefix_db = serializer.deserialize_thrift_object(
            value.value, lsdb_types.PrefixDatabase)
        if delta:
            lines = utils.sprint_prefixes_db_delta(global_prefix_db, prefix_db)
        else:
            lines = utils.sprint_prefixes_db_full(prefix_db)

        if lines:
            print_publication_delta(
                "{}'s prefixes".format(reported_node_name),
                utils.sprint_pub_update(global_publication_db, key, value),
                "\n".join(lines))

        utils.update_global_prefix_db(global_prefix_db, prefix_db)

    def print_adj_delta(self, key, value, delta,
                        global_adj_db, global_publication_db):
        _, reported_node_name = key.split(':', 1)
        new_adj_db = serializer.deserialize_thrift_object(
            value.value, lsdb_types.AdjacencyDatabase)
        if delta:
            old_adj_db = global_adj_db.get(new_adj_db.thisNodeName,
                                           None)
            if old_adj_db is None:
                lines = ["ADJ_DB_ADDED: {}".format(
                    new_adj_db.thisNodeName)]
            else:
                lines = utils.sprint_adj_db_delta(new_adj_db, old_adj_db)
                lines = '\n'.join(lines)
        else:
            _, lines = utils.sprint_adj_db_full(global_adj_db,
                                                new_adj_db, False)

        if lines:
            print_publication_delta(
                "{}'s adjacencies".format(reported_node_name),
                utils.sprint_pub_update(global_publication_db, key, value),
                lines)

        utils.update_global_adj_db(global_adj_db, new_adj_db)

    def print_interface_delta(self, key, value, delta,
                              global_intf_db, global_publication_db):
        _, node_name = key.split(':', 1)
        new_intf_db = utils.interface_db_to_dict(value)

        if delta:
            old_intf_db = global_intf_db.get(node_name, None)

            if old_intf_db is None:
                lines = ["INTERFACE_DB_ADDED: {}".format(node_name)]
            else:
                lines = utils.sprint_interface_db_delta(new_intf_db,
                                                        old_intf_db)
                lines = '\n'.join(lines)
        else:
            lines = utils.sprint_interface_table(new_intf_db)

        if lines:
            print_publication_delta(
                "{}'s interfaces'".format(node_name),
                utils.sprint_pub_update(global_publication_db, key, value),
                lines)

        global_intf_db[new_intf_db.thisNodeName] = new_intf_db

    def get_snapshot(self, delta):
        # get the active network snapshot first, so we can compute deltas
        global_dbs = bunch.Bunch({
            'prefixes': {},
            'adjs': {},
            'interfaces': {},
            'publications': {},  # map(key -> (version, originatorId))
        })

        if delta:
            resp = self.client.dump_all_with_prefix()

            global_dbs.prefixes = utils.build_global_prefix_db(resp)
            global_dbs.adjs = utils.build_global_adj_db(resp)
            global_dbs.interfaces = utils.build_global_interface_db(resp)
            for key, value in resp.keyVals.items():
                global_dbs.publications[key] = (value.version,
                                                value.originatorId)
        return global_dbs


class AllocationsCmd(SetKeyCmd):
    def run_list(self):
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY
        resp = self.client.get_keys([key])
        if key not in resp.keyVals:
            print('Static allocation is not set in KvStore')
        else:
            utils.print_allocations_table(resp.keyVals.get(key).value)

    def run_set(self, node_name, prefix_str):
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY
        prefix = ipnetwork.ip_str_to_prefix(prefix_str)

        # Retrieve previous allocation
        resp = self.client.get_keys([key])
        allocs = None
        if key in resp.keyVals:
            allocs = serializer.deserialize_thrift_object(
                resp.keyVals.get(key).value, alloc_types.StaticAllocation)
        else:
            allocs = alloc_types.StaticAllocation(nodePrefixes={})

        # Return if there is no change
        if allocs.nodePrefixes.get(node_name) == prefix:
            print('No changes needed. {}\'s prefix is already set to {}'
                  .format(node_name, prefix_str))
            return

        # Update value in KvStore
        allocs.nodePrefixes[node_name] = prefix
        value = serializer.serialize_thrift_object(allocs)
        self.run(key, value, 'breeze', None, Consts.CONST_TTL_INF)

    def run_unset(self, node_name):
        key = Consts.STATIC_PREFIX_ALLOC_PARAM_KEY

        # Retrieve previous allocation
        resp = self.client.get_keys([key])
        allocs = None
        if key in resp.keyVals:
            allocs = serializer.deserialize_thrift_object(
                resp.keyVals.get(key).value, alloc_types.StaticAllocation)
        else:
            allocs = alloc_types.StaticAllocation(nodePrefixes={node_name: ''})

        # Return if there need no change
        if node_name not in allocs.nodePrefixes:
            print('No changes needed. {}\'s prefix is not set'
                  .format(node_name))

        # Update value in KvStore
        del allocs.nodePrefixes[node_name]
        value = serializer.serialize_thrift_object(allocs)
        self.run(key, value, 'breeze', None, Consts.CONST_TTL_INF)
