#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import click
from openr.cli.commands import monitor
from openr.cli.utils.options import breeze_option


class MonitorCli(object):
    def __init__(self):
        self.monitor.add_command(CountersCli().counters)
        self.monitor.add_command(MonitorLogs().logs)
        self.monitor.add_command(MonitorStatistics().statistics)

    @click.group()
    @click.pass_context
    def monitor(ctx):  # noqa: B902
        """ CLI tool to peek into Monitor module. """
        pass


class CountersCli(object):
    @click.command()
    @click.option("--json", is_flag=True, help="Output JSON object")
    @click.option(
        "--prefix", default="", help="Only show counters starting with prefix"
    )
    @click.pass_obj
    def counters(cli_opts, prefix, json):  # noqa: B902
        """ Fetch and display OpenR counters """

        monitor.CountersCmd(cli_opts).run(prefix, json)


class MonitorLogs(object):
    @click.command()
    @click.option("--prefix", default="", help="Show log events")
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def logs(cli_opts, prefix, json):  # noqa: B902
        """ Print log events """

        monitor.LogCmd(cli_opts).run(json)


class MonitorStatistics(object):
    @click.command()
    @click.pass_obj
    def statistics(cli_opts):  # noqa: B902
        """ Print counters in pretty format """

        monitor.StatisticsCmd(cli_opts).run()
