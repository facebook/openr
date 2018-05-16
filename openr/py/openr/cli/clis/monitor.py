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

from openr.cli.commands import monitor


class MonitorCli(object):
    def __init__(self):
        self.monitor.add_command(CountersCli().counters)
        self.monitor.add_command(ForceCrashCli().force_crash)
        self.monitor.add_command(MonitorSnoop().snoop)
        self.monitor.add_command(MonitorLogs().logs)
        self.monitor.add_command(MonitorStatistics().statistics)

    @click.group()
    @click.option('--monitor_rep_port', default=None, type=int, help='Monitor rep port')
    @click.pass_context
    def monitor(ctx, monitor_rep_port):  # noqa: B902
        ''' CLI tool to peek into Monitor module. '''

        if monitor_rep_port:
            ctx.obj.monitor_rep_port = monitor_rep_port


class CountersCli(object):

    @click.command()
    @click.option('--json', is_flag=True, help='Output JSON object')
    @click.option('--prefix', default='',
                  help='Only show counters starting with prefix')
    @click.pass_obj
    def counters(cli_opts, prefix, json):  # noqa: B902
        ''' Fetch and display OpenR counters '''

        monitor.CountersCmd(cli_opts).run(prefix, json)


class ForceCrashCli(object):

    @click.command(name='force-crash')
    @click.option('--yes', '-y', is_flag=True,
                  help='Assume yes (non-interactive)')
    @click.pass_obj
    def force_crash(cli_opts, yes):  # noqa: B902
        ''' Trigger force crash of Open/R '''

        monitor.ForceCrashCmd(cli_opts).run(yes)


class MonitorSnoop(object):

    @click.command()
    @click.option('--log/--no-log', default=True,
                  help='Snoop on log')
    @click.option('--counters/--no-counters', default=True,
                  help='Snoop on counters')
    @click.option('--delta/--no-delta', default=True,
                  help='Output incremental changes')
    @click.option('--duration', default=0,
                  help='How long to snoop for. Default is infinite')
    @click.pass_obj
    def snoop(cli_opts, log, counters, delta, duration):  # noqa: B902
        ''' Print changed counters '''

        monitor.SnoopCmd(cli_opts).run(log, counters, delta, duration)


class MonitorLogs(object):

    @click.command()
    @click.option('--prefix', default="", help='Show log events')
    @click.option('--json/--no-json', default=False, help='Dump in JSON format')
    @click.pass_obj
    def logs(cli_opts, prefix, json):  # noqa: B902
        ''' Print log events '''

        monitor.LogCmd(cli_opts).run(json)


class MonitorStatistics(object):

    @click.command()
    @click.pass_obj
    def statistics(cli_opts):  # noqa: B902
        ''' Print counters in pretty format '''

        monitor.StatisticsCmd(cli_opts).run()
