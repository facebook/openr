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

import click
import zmq

from thrift.protocol.TCompactProtocol import TCompactProtocolFactory
from thrift.protocol.TJSONProtocol import TJSONProtocolFactory

from openr.cli.commands import kvstore
from openr.utils.consts import Consts
from openr.cli.utils.utils import parse_nodes


class KvStoreContext(object):
    def __init__(self, verbose, zmq_ctx, host, timeout,
                 kv_rep_port, kv_pub_port, lm_cmd_port,
                 json, enable_color):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
            :param host string: the openr router host
            :param kv_rep_port int: the kv-store port
            :param json bool: whether to use JSON proto or Compact for thrift
            :param enable_color bool: whether to turn on coloring display
        '''

        self.verbose = verbose
        self.host = host
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx
        self.enable_color = enable_color

        self.kv_rep_port = kv_rep_port
        self.kv_pub_port = kv_pub_port
        self.lm_cmd_port = lm_cmd_port

        self.proto_factory = (TJSONProtocolFactory if json
                              else TCompactProtocolFactory)


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

    @click.group()
    @click.option('--kv_rep_port', default=Consts.KVSTORE_REP_PORT,
                  help='KV store rep port')
    @click.option('--kv_pub_port', default=Consts.KVSTORE_PUB_PORT,
                  help='KV store pub port')
    @click.option('--json/--no-json', default=False,
                  help='Use JSON serializer')
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def kvstore(ctx, kv_rep_port, kv_pub_port, json, verbose):  # noqa: B902
        ''' CLI tool to peek into KvStore module. '''

        ctx.obj = KvStoreContext(
            verbose, zmq.Context(),
            ctx.obj.hostname,
            ctx.obj.timeout,
            ctx.obj.ports_config.get('kv_rep_port', None) or kv_rep_port,
            ctx.obj.ports_config.get('kv_pub_port', None) or kv_pub_port,
            ctx.obj.ports_config.get('lm_cmd_port', None) or
            Consts.LINK_MONITOR_CMD_PORT,
            json,
            ctx.obj.enable_color)


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
    @click.option('--prefix', default='', help='string to filter keys')
    @click.option('--ttl/--no-ttl', default=False,
                  help='Show ttl value and version as well')
    @click.pass_obj
    def keys(cli_opts, prefix, ttl):  # noqa: B902
        ''' dump all available keys '''

        kvstore.KeysCmd(cli_opts).run(prefix, ttl)


class KeyValsCli(object):

    @click.command()
    @click.argument('keys', nargs=-1)
    @click.pass_obj
    def keyvals(cli_opts, keys):  # noqa: B902
        ''' get values of input keys '''

        kvstore.KeyValsCmd(cli_opts).run(keys)


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
    @click.pass_obj
    def interfaces(cli_opts, nodes, json):  # noqa: B902
        ''' dump interface information '''

        nodes = parse_nodes(cli_opts.host, nodes, cli_opts.lm_cmd_port)
        kvstore.InterfacesCmd(cli_opts).run(nodes, json)


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

        ttl_ms = ttl * 1000
        kvstore.SetKeyCmd(cli_opts).run(key, value, originator, version, ttl_ms)


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
    @click.pass_obj
    def topology(cli_opts, node, bidir, output_file):  # noqa: B902
        ''' generates an image file with a visualization of the openr topology
        '''

        kvstore.TopologyCmd(cli_opts).run(node, bidir, output_file)


class SnoopCli(object):

    @click.command()
    @click.option('--delta/--no-delta', default=True,
                  help='Output incremental changes')
    @click.option('--ttl/--no-ttl', default=False,
                  help='Print ttl updates')
    @click.option('--regex', default='',
                  help='Snoop on keys matching filter')
    @click.pass_obj
    def snoop(cli_opts, delta, ttl, regex):  # noqa: B902
        ''' Snoop on KV-store updates in the network. We are primarily
            looking at the adj/prefix announcements.
        '''

        kvstore.SnoopCmd(cli_opts).run(delta, ttl, regex)
