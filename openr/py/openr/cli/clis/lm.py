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

from openr.cli.commands import lm
from openr.utils.consts import Consts


class LMContext(object):
    def __init__(self, verbose, zmq_ctx, host, timeout, lm_cmd_port, json,
                 enable_color):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
            :param host string: the openr router host
            :param lm_cmd_port int: the link monitor module port
            :param json bool: whether to use JSON proto or Compact for thrift
            :param enable_color bool: whether to turn on coloring display
        '''

        self.verbose = verbose
        self.host = host
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx
        self.enable_color = enable_color

        self.lm_cmd_port = lm_cmd_port

        self.proto_factory = (TJSONProtocolFactory if json
                              else TCompactProtocolFactory)


class LMCli(object):

    def __init__(self):

        self.lm.add_command(LMLinksCli().links)
        self.lm.add_command(SetNodeOverloadCli().set_node_overload,
                            name='set-node-overload')
        self.lm.add_command(UnsetNodeOverloadCli().unset_node_overload,
                            name='unset-node-overload')
        self.lm.add_command(SetLinkOverloadCli().set_link_overload,
                            name='set-link-overload')
        self.lm.add_command(UnsetLinkOverloadCli().unset_link_overload,
                            name='unset-link-overload')
        self.lm.add_command(SetLinkMetricCli().set_link_metric,
                            name='set-link-metric')
        self.lm.add_command(UnsetLinkMetricCli().unset_link_metric,
                            name='unset-link-metric')

    @click.group()
    @click.option('--lm_cmd_port', default=Consts.LINK_MONITOR_CMD_PORT,
                  help='Link Monitor port')
    @click.option('--json/--no-json', default=False, help='Use JSON serializer')
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def lm(ctx, lm_cmd_port, json, verbose):  # noqa: B902
        ''' CLI tool to peek into Link Monitor module. '''

        ctx.obj = LMContext(
            verbose, zmq.Context(),
            ctx.obj.hostname,
            ctx.obj.timeout,
            ctx.obj.ports_config.get('lm_cmd_port', None) or lm_cmd_port,
            json,
            ctx.obj.enable_color)


class LMLinksCli(object):

    @click.command()
    @click.option('--all/--no-all', default=False,
                  help='Show all links including ones without addresses')
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def links(cli_opts, all, json):  # noqa: B902
        ''' Dump all known links of the current host '''

        lm.LMLinksCmd(cli_opts).run(all, json)


class SetNodeOverloadCli(object):

    @click.command()
    @click.pass_obj
    def set_node_overload(cli_opts):  # noqa: B902
        ''' Set overload bit to stop transit traffic through node. '''

        lm.SetNodeOverloadCmd(cli_opts).run()


class UnsetNodeOverloadCli(object):

    @click.command()
    @click.pass_obj
    def unset_node_overload(cli_opts):  # noqa: B902
        ''' Unset overload bit to resume transit traffic through node. '''

        lm.UnsetNodeOverloadCmd(cli_opts).run()


class SetLinkOverloadCli(object):

    @click.command()
    @click.argument('interface')
    @click.pass_obj
    def set_link_overload(cli_opts, interface):  # noqa: B902
        ''' Set overload bit for a link. Transit traffic will be drained. '''

        lm.SetLinkOverloadCmd(cli_opts).run(interface)


class UnsetLinkOverloadCli(object):

    @click.command()
    @click.argument('interface')
    @click.pass_obj
    def unset_link_overload(cli_opts, interface):  # noqa: B902
        ''' Unset overload bit for a link to allow transit traffic. '''

        lm.UnsetLinkOverloadCmd(cli_opts).run(interface)


class SetLinkMetricCli(object):

    @click.command()
    @click.argument('interface')
    @click.argument('metric')
    @click.pass_obj
    def set_link_metric(cli_opts, interface, metric):  # noqa: B902
        '''
        Set custom metric value for a link. You can use high link metric value
        to emulate soft-drain behaviour.
        '''

        lm.SetLinkMetricCmd(cli_opts).run(interface, metric)


class UnsetLinkMetricCli(object):

    @click.command()
    @click.argument('interface')
    @click.pass_obj
    def unset_link_metric(cli_opts, interface):  # noqa: B902
        '''
        Unset previously set custom metric value on the interface.
        '''

        lm.UnsetLinkMetricCmd(cli_opts).run(interface)
