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

from openr.cli.commands import prefix_mgr
from openr.utils.consts import Consts


class PrefixMgrContext(object):
    def __init__(self, verbose, zmq_ctx, host, timeout,
                 prefix_mgr_cmd_port, json):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
            :param host string: the openr router host
            :param kv_rep_port int: the kv-store port
            :para json bool: whether to use JSON proto or Compact for thrift
        '''

        self.verbose = verbose
        self.host = host
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx

        self.prefix_mgr_cmd_port = prefix_mgr_cmd_port

        self.proto_factory = (TJSONProtocolFactory if json
                              else TCompactProtocolFactory)


class PrefixMgrCli(object):
    def __init__(self):
        self.prefixmgr.add_command(WithdrawCli().withdraw)
        self.prefixmgr.add_command(AdvertiseCli().advertise)
        self.prefixmgr.add_command(ViewCli().view)
        self.prefixmgr.add_command(SyncCli().sync)

    @click.group()
    @click.option('--prefix_mgr_cmd_port', default=Consts.PREFIX_MGR_CMD_PORT,
                  help='Prefix Manager port')
    @click.option('--json/--no-json', default=False,
                  help='Use JSON serializer')
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def prefixmgr(ctx, prefix_mgr_cmd_port, json, verbose):  # noqa: B902
        ''' CLI tool to peek into Prefix Manager module. '''

        ctx.obj = PrefixMgrContext(
            verbose, zmq.Context(),
            ctx.obj.hostname,
            ctx.obj.timeout,
            ctx.obj.ports_config.get('prefix_mgr_cmd_port', None) or
            prefix_mgr_cmd_port,
            json)


class WithdrawCli(object):

    @click.command()
    @click.argument('prefixes', nargs=-1)
    @click.pass_obj
    def withdraw(cli_opts, prefixes):  # noqa: B902
        ''' Withdraw the prefixes being advertised from this node '''

        prefix_mgr.WithdrawCmd(cli_opts).run(prefixes)


class AdvertiseCli(object):

    @click.command()
    @click.argument('prefixes', nargs=-1)
    @click.option('--prefix-type', '-t', default='BREEZE',
                  help='Type or client-ID associated with prefix.')
    @click.pass_obj
    def advertise(cli_opts, prefixes, prefix_type):  # noqa: B902
        ''' Advertise the prefixes from this node with specific type '''

        prefix_mgr.AdvertiseCmd(cli_opts).run(prefixes, prefix_type)


class SyncCli(object):

    @click.command()
    @click.argument('prefixes', nargs=-1)
    @click.option('--prefix-type', '-t', default='BREEZE',
                  help='Type or client-ID associated with prefix.')
    @click.pass_obj
    def sync(cli_opts, prefixes, prefix_type):  # noqa: B902
        ''' Sync the prefixes from this node with specific type '''

        prefix_mgr.SyncCmd(cli_opts).run(prefixes, prefix_type)


class ViewCli(object):

    @click.command()
    @click.pass_obj
    def view(cli_opts):  # noqa: B902
        ''' View the prefix of this node '''

        prefix_mgr.ViewCmd(cli_opts).run()
