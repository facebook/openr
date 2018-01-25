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

from openr.cli.commands import perf


class PerfCli(object):
    def __init__(self):
        self.perf.add_command(ViewFibCli().fib)

    @click.group()
    @click.option('--fib_cmd_port', default=None, help='Fib rep port')
    @click.pass_context
    def perf(ctx, fib_cmd_port):  # noqa: B902
        ''' CLI tool to view latest perf log of each module. '''

        if fib_cmd_port:
            ctx.obj.fib_cmd_port = fib_cmd_port


class ViewFibCli(object):

    @click.command()
    @click.pass_obj
    def fib(cli_opts):  # noqa: B902
        ''' View latest perf log of fib module from this node '''

        perf.ViewFibCmd(cli_opts).run()
