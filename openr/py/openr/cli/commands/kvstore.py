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
from openr.clients import kvstore_client, kvstore_subscriber
from openr.cli.utils import utils
from openr.utils.consts import Consts
from openr.utils import printing
from openr.utils.serializer import deserialize_thrift_object

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
            prefix_db = deserialize_thrift_object(
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
                return utils.sprint_addr(prefix_entry.prefix.prefixAddress.addr)

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
    def run(self, json_fmt, prefix, ttl):
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
            if ttl:
                if value.ttl != Consts.CONST_TTL_INF:
                    ttlStr = str(datetime.timedelta(milliseconds=value.ttl))
                else:
                    ttlStr = "Inf"
                rows.append([key, value.originatorId, value.version, value.hash,
                              ttlStr, value.ttlVersion])
            else:
                rows.append([key, value.originatorId, value.version, value.hash])

        caption = "Available keys in KvStore"
        column_labels = ["Key", "OriginatorId", "Version", "Hash"]
        if ttl:
            column_labels = column_labels + ["TTL (HH:MM:SS)", "TTL Version"]

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
            return deserialize_thrift_object(value.value, options[prefix_type])
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
            prefix_db = deserialize_thrift_object(value.value,
                                                  lsdb_types.PrefixDatabase)

            prefix_strs = utils.sprint_prefixes_db_full(prefix_db, True)

            marker = '* ' if prefix_db.thisNodeName == host_id else '> '
            row = ["{}{}".format(marker, prefix_db.thisNodeName)]
            row.extend(prefix_strs)
            rows.append(row)

        rows = []
        self.iter_publication(rows, resp, set(['all']), _parse_nodes)

        label = ['Node', 'V6-Loopback']
        # if any node has a v4 loopback addr, we should have the v4-addr column
        if any(len(row) > 2 for row in rows):
            label.append('V4-Loopback')

        print(printing.render_horizontal_table(rows, label))


class AdjCmd(KvStoreCmd):
    def run(self, nodes, bidir, json):
        publication = self.client.dump_all_with_prefix(Consts.ADJ_DB_MARKER)
        adjs_map = utils.adj_dbs_to_dict(publication, nodes, bidir,
                                         self.iter_publication)
        if json:
            utils.print_adjs_json(adjs_map)
        else:
            utils.print_adjs_table(adjs_map, self.enable_color)


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
                nodes = all_nodes_to_ips.keys()
            host_id = utils.get_connected_node_name(self.host, self.lm_cmd_port)
            if host_id in nodes:
                nodes.remove(host_id)

            our_kvs = self.client.dump_all_with_prefix().keyVals
            kv_dict = self.dump_nodes_kvs(nodes, all_nodes_to_ips)
            for node in kv_dict:
                self.compare(our_kvs, kv_dict[node], host_id, node)

        else:
            nodes = all_nodes_to_ips.keys()
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
        for (key, value) in our_kvs.items():
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
            prefix_db = deserialize_thrift_object(value.value,
                                                  lsdb_types.PrefixDatabase)
            other_prefix_db = deserialize_thrift_object(other_val.value,
                                                        lsdb_types.PrefixDatabase)
            other_prefix_set = {}
            utils.update_global_prefix_db(other_prefix_set, other_prefix_db)
            lines = utils.sprint_prefixes_db_delta(other_prefix_set, prefix_db)

        elif key.startswith(Consts.ADJ_DB_MARKER):
            adj_db = deserialize_thrift_object(value.value,
                                               lsdb_types.AdjacencyDatabase)
            other_adj_db = deserialize_thrift_object(value.value,
                                                     lsdb_types.AdjacencyDatabase)
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
            row.append('Public Key: {}'.format(value.publicKey.encode("hex")))
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
        if response != 'OK':
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
        if response != 'OK':
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
            signature.update(str(value.hash))

        print('sha256: {}'.format(signature.hexdigest()))


class TopologyCmd(KvStoreCmd):
    def run(self, node, bidir, output_file):

        try:
            import matplotlib.pyplot as plt
            import networkx as nx
        except Exception:
            print("matplotlib and networkx needed for drawing. Skipping")
            sys.exit(1)

        publication = self.client.dump_all_with_prefix(Consts.ADJ_DB_MARKER)
        nodes = self.get_node_to_ips().keys() if not node else [node]
        adjs_map = utils.adj_dbs_to_dict(publication, nodes, bidir,
                                         self.iter_publication)
        G = nx.Graph()
        for this_node_name, db in adjs_map.items():
            for adj in db['adjacencies']:
                G.add_edge(this_node_name, adj['otherNodeName'], **adj)

        # hack to get nice fabric
        # XXX: FB Specific
        pos = {}
        eswx = 0
        sswx = 0
        fswx = 0
        rswx = 0
        for node in G.nodes():
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
            plt.figure(figsize=(maxswx * 0.3, 8))
        else:
            plt.figure(figsize=(len(G.nodes()) * 2, len(G.nodes()) * 2))
            pos = nx.spring_layout(G)
        plt.axis('off')
        nx.draw_networkx(G, pos, ax=None, alpha=0.5, font_size=8)
        if node:
            edge_labels = dict([((u, v), str(d['ifName']) + ' metric: ' +
                               str(d['metric']))
                               for u, v, d in G.edges(data=True)])
            nx.draw_networkx_edge_labels(G, pos, edge_labels=edge_labels,
                                         font_size=6)

        print('Saving topology to file => {}'.format(output_file))
        plt.savefig(output_file)


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

        for (key, value) in msg.keyVals.items():
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
        prefix_db = deserialize_thrift_object(value.value,
                                              lsdb_types.PrefixDatabase)
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
        new_adj_db = deserialize_thrift_object(value.value,
                                               lsdb_types.AdjacencyDatabase)
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
            for (key, value) in resp.keyVals.items():
                global_dbs.publications[key] = (value.version,
                                                value.originatorId)
        return global_dbs
