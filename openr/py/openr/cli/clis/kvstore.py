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
from builtins import object

import click

from openr.cli.commands import kvstore
from openr.utils.consts import Consts
from openr.cli.utils.utils import parse_nodes


class KvStoreCli(object):
    def __init__(self):
        self.kvstore.add_command(PrefixesCli().prefixes)
        self.kvstore.add_command(AdjCli().adj)
        self.kvstore.add_command(InterfacesCli().interfaces)
        self.kvstore.add_command(NodesCli().nodes)
        self.kvstore.add_command(KeysCli().keys)
        self.kvstore.add_command(KeyValsCli().keyvals)
        self.kvstore.add_command(KvCompareCli().kv_compare, name='kv-compare')
        self.kvstore.add_command(PeersCli().peers)
        self.kvstore.add_command(EraseKeyCli().erase_key, name='erase-key')
        self.kvstore.add_command(SetKeyCli().set_key, name='set-key')
        self.kvstore.add_command(KvSignatureCli().kv_signature, name='kv-signature')
        self.kvstore.add_command(TopologyCli().topology)
        self.kvstore.add_command(SnoopCli().snoop)
        self.kvstore.add_command(AllocationsCli().list, name='alloc-list')
        self.kvstore.add_command(AllocationsCli().set, name='alloc-set')
        self.kvstore.add_command(AllocationsCli().unset, name='alloc-unset')

    @click.group()
    @click.option('--kv_rep_port', default=None, type=int, help='KV store rep port')
    @click.option('--kv_pub_port', default=None, type=int, help='KV store pub port')
    @click.pass_context
    def kvstore(ctx, kv_rep_port, kv_pub_port):  # noqa: B902
        ''' CLI tool to peek into KvStore module. '''

        if kv_pub_port:
            ctx.obj.kv_pub_port = kv_pub_port
        if kv_rep_port:
            ctx.obj.kv_rep_port = kv_rep_port


class PrefixesCli(object):

    @click.command()
    @click.option('--nodes', default='',
                  help='Dump prefixes for a list of nodes. Default will dump host\'s '
                       'prefixes. Dump prefixes for all nodes if \'all\' is given.')
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def prefixes(cli_opts, nodes, json):  # noqa: B902
        ''' show the prefixes in the network '''

        nodes = parse_nodes(cli_opts.host, nodes, cli_opts.lm_cmd_port)
        kvstore.PrefixesCmd(cli_opts).run(nodes, json)


class KeysCli(object):

    @click.command()
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.option('--prefix', default='', help='string to filter keys')
    @click.option('--originator', default=None, help='originator string to filter keys')
    @click.option('--ttl/--no-ttl', default=False,
                  help='Show ttl value and version as well')
    @click.pass_obj
    def keys(cli_opts, json, prefix, originator, ttl):  # noqa: B902
        ''' dump all available keys '''

        kvstore.KeysCmd(cli_opts).run(json, prefix, originator, ttl)


class KeyValsCli(object):

    @click.command()
    @click.argument('keys', nargs=-1, required=True)
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def keyvals(cli_opts, keys, json):  # noqa: B902
        ''' get values of input keys '''

        kvstore.KeyValsCmd(cli_opts).run(keys, json)


class NodesCli(object):

    @click.command()
    @click.pass_obj
    def nodes(cli_opts):  # noqa: B902
        ''' show nodes info '''

        kvstore.NodesCmd(cli_opts).run()


class AdjCli(object):

    @click.command()
    @click.option('--nodes', default='',
                  help='Get adjacencies for specified of nodes. Default will '
                       'get localhost\'s adjacencies. Get adjacencies for all '
                       'nodes if \'all\' is given.')
    @click.option('--bidir/--no-bidir', default=True,
                  help='Only bidir adjacencies')
    @click.option('--json/--no-json', default=False, help='Dump in JSON format')
    @click.pass_obj
    def adj(cli_opts, nodes, bidir, json):  # noqa: B902
        ''' dump the link-state adjacencies '''

        nodes = parse_nodes(cli_opts.host, nodes, cli_opts.lm_cmd_port)
        kvstore.AdjCmd(cli_opts).run(nodes, bidir, json)


class InterfacesCli(object):

    @click.command()
    @click.option('--nodes', default='',
                  help='Get interface database of nodes. Default will get '
                       'host\'s interfaces. Get interfaces for all nodes if '
                       '\'all\' is give.')
    @click.option('--json/--no-json', default=False, help='Dump in JSON format')
    @click.option('--all/--no-all', default=False,
                  help='Show all links including ones without addresses')
    @click.pass_obj
    def interfaces(cli_opts, nodes, json, all):  # noqa: B902
        ''' dump interface information '''

        nodes = parse_nodes(cli_opts.host, nodes, cli_opts.lm_cmd_port)
        kvstore.InterfacesCmd(cli_opts).run(nodes, json, all)


class KvCompareCli(object):

    @click.command()
    @click.option('--nodes', default='',
                  help='Kv-compare the current host with a list of nodes. '
                  'Compare with all the other nodes if \'all\' is given. '
                  'Default will kv-compare against each peer.')
    @click.pass_obj
    def kv_compare(cli_opts, nodes):  # noqa: B902
        ''' get the kv store delta '''

        kvstore.KvCompareCmd(cli_opts).run(nodes)


class PeersCli(object):

    @click.command()
    @click.pass_obj
    def peers(cli_opts):  # noqa: B902
        ''' show the KV store peers of the node '''

        kvstore.PeersCmd(cli_opts).run()


class EraseKeyCli(object):

    @click.command()
    @click.argument('key')
    @click.pass_obj
    def erase_key(cli_opts, key):  # noqa: B902
        ''' erase key from kvstore '''

        kvstore.EraseKeyCmd(cli_opts).run(key)


class SetKeyCli(object):

    @click.command()
    @click.argument('key')
    @click.argument('value')
    @click.option('--originator', default='breeze', help='Originator ID')
    @click.option('--version', default=None,
                  help='Version. If not set, override existing key if any')
    @click.option('--ttl', default=Consts.CONST_TTL_INF,
                  help='TTL in seconds. Default is infinite')
    @click.pass_obj
    def set_key(cli_opts, key, value, originator, version, ttl):  # noqa: B902
        ''' Set a custom key into KvStore '''

        if ttl != Consts.CONST_TTL_INF:
            ttl = ttl * 1000
        kvstore.SetKeyCmd(cli_opts).run(key, value, originator, version, ttl)


class KvSignatureCli(object):

    @click.command()
    @click.option('--prefix', default="", help='Limit the keys included '
                  'in the signature computation to those that begin with '
                  'the given prefix')
    @click.pass_obj
    def kv_signature(cli_opts, prefix):  # noqa: B902
        ''' Returns a signature of the contents of the KV store for comparison
        with other nodes.  In case of mismatch, use kv-compare to analyze
        differences
        '''

        kvstore.KvSignatureCmd(cli_opts).run(prefix)


class TopologyCli(object):

    @click.command()
    @click.option('--node', default='',
                  help='Show adjacencies for specific node, '
                       'this yields more adjacency info')
    @click.option('--bidir/--no-bidir', default=True,
                  help='Only bidir adjacencies')
    @click.option('--output-file', default=Consts.TOPOLOGY_OUTPUT_FILE,
                  help='File to write .png image to '
                       '(default is \'/tmp/openr-topology.png\')')
    @click.option('--edge-label/--no-edge-label', default=True,
                  help='Exclude edge labels')
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def topology(cli_opts, node, bidir,  # noqa: B902
                 output_file, edge_label, json):
        ''' generates an image file with a visualization of the openr topology
        '''

        kvstore.TopologyCmd(cli_opts).run(
            node, bidir, output_file, edge_label, json)


class SnoopCli(object):

    @click.command()
    @click.option('--delta/--no-delta', default=True,
                  help='Output incremental changes')
    @click.option('--ttl/--no-ttl', default=False,
                  help='Print ttl updates')
    @click.option('--regex', default='',
                  help='Snoop on keys matching filter')
    @click.option('--duration', default=0,
                  help='How long to snoop for. Default is infinite')
    @click.pass_obj
    def snoop(cli_opts, delta, ttl, regex, duration):  # noqa: B902
        ''' Snoop on KV-store updates in the network. We are primarily
            looking at the adj/prefix announcements.
        '''

        kvstore.SnoopCmd(cli_opts).run(delta, ttl, regex, duration)


class AllocationsCli(object):

    @click.command()
    @click.pass_obj
    def list(cli_opts):  # noqa: B902
        ''' View static allocations set in KvStore '''

        kvstore.AllocationsCmd(cli_opts).run_list()

    @click.command()
    @click.argument('node', nargs=1, required=True)
    @click.argument('prefix', nargs=1, required=True)
    @click.pass_obj
    def set(cli_opts, node, prefix):  # noqa: B902
        ''' Set/Update prefix allocation for a certain node '''

        kvstore.AllocationsCmd(cli_opts).run_set(node, prefix)

    @click.command()
    @click.argument('node', nargs=1, required=True)
    @click.pass_obj
    def unset(cli_opts, node):  # noqa: B902
        ''' Unset prefix allocation for a certain node '''

        kvstore.AllocationsCmd(cli_opts).run_unset(node)
