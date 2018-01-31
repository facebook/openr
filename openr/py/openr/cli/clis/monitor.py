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

from openr.cli.commands import monitor


class MonitorCli(object):
    def __init__(self):
        self.monitor.add_command(CountersCli().counters)
        self.monitor.add_command(ForceCrashCli().force_crash)

    @click.group()
    @click.option('--monitor_rep_port', default=None, type=int, help='Monitor rep port')
    @click.pass_context
    def monitor(ctx, monitor_rep_port):  # noqa: B902
        ''' CLI tool to peek into Monitor module. '''

        if monitor_rep_port:
            ctx.obj.monitor_rep_port = monitor_rep_port


class CountersCli(object):

    @click.command()
    @click.option('--prefix', default='',
                  help='Only show counters starting with prefix')
    @click.pass_obj
    def counters(cli_opts, prefix):  # noqa: B902
        ''' Fetch and display OpenR counters '''

        monitor.CountersCmd(cli_opts).run(prefix)


class ForceCrashCli(object):

    @click.command(name='force-crash')
    @click.option('--yes', '-y', is_flag=True,
                  help='Assume yes (non-interactive)')
    @click.pass_obj
    def force_crash(cli_opts, yes):  # noqa: B902
        ''' Trigger force crash of Open/R '''

        monitor.ForceCrashCmd(cli_opts).run(yes)
