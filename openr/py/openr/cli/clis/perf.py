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

from openr.cli.commands import perf
from openr.utils.consts import Consts


class PerfContext(object):
    def __init__(self, verbose, zmq_ctx, host, timeout, fib_cmd_port):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
            :param host string: the openr router host
            :param fib_cmd_port int: the fib rep port
        '''

        self.verbose = verbose
        self.host = host
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx
        self.fib_cmd_port = fib_cmd_port
        self.proto_factory = Consts.PROTO_FACTORY


class PerfCli(object):
    def __init__(self):
        self.perf.add_command(ViewFibCli().fib)

    @click.group()
    @click.option('--fib_cmd_port', default=Consts.FIB_REP_PORT,
                  help='Fib rep port')
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def perf(ctx, fib_cmd_port, verbose):  # noqa: B902
        ''' CLI tool to view latest perf log of each module. '''

        ctx.obj = PerfContext(
            verbose, zmq.Context(),
            ctx.obj.hostname,
            ctx.obj.timeout,
            ctx.obj.ports_config.get('fib_cmd_port', None) or fib_cmd_port)


class ViewFibCli(object):

    @click.command()
    @click.pass_obj
    def fib(cli_opts):  # noqa: B902
        ''' View latest perf log of fib module from this node '''

        perf.ViewFibCmd(cli_opts).run()
