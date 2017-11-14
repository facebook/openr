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

from openr.cli.commands import monitor
from openr.utils.consts import Consts


class MonitorContext(object):
    def __init__(self, verbose, zmq_ctx, host, timeout,
                 monitor_rep_port, lm_cmd_port, json):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
            :param host string: the openr router host
            :para json bool: whether to use JSON proto or Compact for thrift
        '''

        self.verbose = verbose
        self.host = host
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx

        self.monitor_rep_port = monitor_rep_port
        self.lm_cmd_port = lm_cmd_port

        self.proto_factory = (TJSONProtocolFactory if json
                              else TCompactProtocolFactory)


class MonitorCli(object):
    def __init__(self):
        self.monitor.add_command(CountersCli().counters)

    @click.group()
    @click.option('--monitor_rep_port', default=Consts.MONITOR_REP_PORT,
                  help='Monitor rep port')
    @click.option('--json/--no-json', default=False,
                  help='Use JSON serializer')
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def monitor(ctx, monitor_rep_port, json, verbose):  # noqa: B902
        ''' CLI tool to peek into Monitor module. '''

        ctx.obj = MonitorContext(
            verbose, zmq.Context(),
            ctx.obj.hostname,
            ctx.obj.timeout,
            ctx.obj.ports_config.get('monitor_rep_port', None) or monitor_rep_port,
            ctx.obj.ports_config.get('lm_cmd_port', None) or
            Consts.LINK_MONITOR_CMD_PORT,
            json)


class CountersCli(object):

    @click.command()
    @click.option('--prefix', default='',
                  help='Only show counters starting with prefix')
    @click.pass_obj
    def counters(cli_opts, prefix):  # noqa: B902
        ''' Fetch and display OpenR counters '''

        monitor.CountersCmd(cli_opts).run(prefix)
