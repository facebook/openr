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

from openr.cli.commands import health_checker
from openr.utils.consts import Consts


class HealthCheckerContext(object):
    def __init__(self, verbose, zmq_ctx, host, timeout,
                 health_checker_cmd_port, json):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
            :param host string: the openr router host
            :param health_checker_cmd_port int: the health checker rep port
            :para json bool: whether to use JSON proto or Compact for thrift
        '''

        self.verbose = verbose
        self.host = host
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx

        self.health_checker_cmd_port = health_checker_cmd_port

        self.proto_factory = (TJSONProtocolFactory if json
                              else TCompactProtocolFactory)


class HealthCheckerCli(object):
    def __init__(self):
        self.healthchecker.add_command(PeekCli().peek)

    @click.group()
    @click.option('--health_checker_cmd_port', default=Consts.HEALTH_CHECKER_CMD_PORT,
                  help='Health Checker port')
    @click.option('--json/--no-json', default=False,
                  help='Use JSON serializer')
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def healthchecker(ctx, health_checker_cmd_port, json, verbose):  # noqa: B902
        ''' CLI tool to peek into Health Checker module. '''

        ctx.obj = HealthCheckerContext(
            verbose, zmq.Context(),
            ctx.obj.hostname,
            ctx.obj.timeout,
            ctx.obj.ports_config.get('health_checker_cmd_port', None) or
            health_checker_cmd_port,
            json)


class PeekCli(object):

    @click.command()
    @click.pass_obj
    def peek(cli_opts):  # noqa: B902
        ''' View the health checker result from this node '''

        health_checker.PeekCmd(cli_opts).run()
