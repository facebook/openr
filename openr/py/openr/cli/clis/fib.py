#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import sys
from typing import List

import click
from bunch import Bunch
from openr.cli.commands import fib
from openr.cli.utils.options import breeze_option


class FibCli(object):
    def __init__(self):
        # ATTN: get unicast + mpls routes installed on platform, aka,
        # fibAgent via thrift port.
        self.fib.add_command(FibRoutesInstalledCli().routes, name="routes-installed")

        # ATTN: get unicast and mpls routes respectively from Open/R's
        # software state, aka, Fib module.
        self.fib.add_command(FibUnicastRoutesCli().routes, name="unicast-routes")
        self.fib.add_command(FibMplsRoutesCli().routes, name="mpls-routes")

        self.fib.add_command(FibCountersCli().counters, name="counters")
        self.fib.add_command(FibAddRoutesCli().add_routes, name="add")
        self.fib.add_command(FibDelRoutesCli().del_routes, name="del")
        self.fib.add_command(FibSyncRoutesCli().sync_routes, name="sync")
        self.fib.add_command(FibSnoopCli().snoop)
        self.fib.add_command(FibValidateRoutesCli().validate)
        self.fib.add_command(StreamSummaryCli()._stream_summary, name="stream-summary")

    @click.group()
    @breeze_option("--fib_agent_port", type=int, help="Fib thrift server port")
    @breeze_option("--client-id", type=int, help="FIB Client ID")
    @click.pass_context
    def fib(ctx, fib_agent_port, client_id):  # noqa: B902
        """CLI tool to peek into Fib module."""
        pass


class FibCountersCli(object):
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def counters(cli_opts, json):  # noqa: B902
        """Get various counters on fib agent"""

        return_code = fib.FibCountersCmd(cli_opts).run(json)
        sys.exit(return_code)


class FibRoutesInstalledCli(object):
    @click.command()
    @click.option(
        "--prefixes",
        "-p",
        type=click.STRING,
        multiple=True,
        help="Get route for specific IPs or Prefixes.",
    )
    @click.option(
        "--labels",
        "-l",
        type=click.INT,
        multiple=True,
        help="Get route for specific labels.",
    )
    @click.option(
        "--client-id",
        type=click.INT,
        help="Retrieve routes for client. Defaults to 786 (Open/R). Use 0 for BGP",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def routes(
        cli_opts: Bunch,  # noqa: B902
        prefixes: List[str],
        labels: List[int],
        client_id: int,
        json: bool,
    ):
        """Get and print all the routes on fib agent"""

        return_code = fib.FibRoutesInstalledCmd(cli_opts).run(
            prefixes, labels, json, client_id
        )
        sys.exit(return_code)


class FibUnicastRoutesCli(object):
    @click.command()
    @click.argument("prefix_or_ip", nargs=-1)
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def routes(cli_opts, prefix_or_ip, json):  # noqa: B902
        """Request unicast routing table of the current host"""

        fib.FibUnicastRoutesCmd(cli_opts).run(prefix_or_ip, json)


class FibMplsRoutesCli(object):
    @click.command()
    @click.argument("labels", nargs=-1)
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def routes(cli_opts, labels, json):  # noqa: B902
        """Request Mpls routing table of the current host"""

        fib.FibMplsRoutesCmd(cli_opts).run(labels, json)


class FibAddRoutesCli(object):
    @click.command()
    @click.argument("prefixes")  # Comma separated list of prefixes
    @click.argument("nexthops")  # Comma separated list of nexthops
    @click.pass_obj
    def add_routes(cli_opts, prefixes, nexthops):  # noqa: B902
        """Add new routes in FIB"""

        fib.FibAddRoutesCmd(cli_opts).run(prefixes, nexthops)


class FibDelRoutesCli(object):
    @click.command()
    @click.argument("prefixes")  # Comma separated list of prefixes
    @click.pass_obj
    def del_routes(cli_opts, prefixes):  # noqa: B902
        """Delete routes from FIB"""

        fib.FibDelRoutesCmd(cli_opts).run(prefixes)


class FibSyncRoutesCli(object):
    @click.command()
    @click.argument("prefixes")  # Comma separated list of prefixes
    @click.argument("nexthops")  # Comma separated list of nexthops
    @click.pass_obj
    def sync_routes(cli_opts, prefixes, nexthops):  # noqa: B902
        """Re-program FIB with specified routes. Delete all old ones"""

        fib.FibSyncRoutesCmd(cli_opts).run(prefixes, nexthops)


class FibValidateRoutesCli(object):
    @click.command()
    @click.pass_obj
    def validate(cli_opts):  # noqa: B902
        """Validator to check that all routes as computed by Decision"""

        sys.exit(fib.FibValidateRoutesCmd(cli_opts).run(cli_opts))


class FibSnoopCli(object):
    @click.command()
    @click.option(
        "--duration",
        "-d",
        default=0,
        help="How long to snoop in seconds. Default is infinite",
    )
    @click.option(
        "--initial-dump/--no-initial-dump", default=False, help="Output initial full DB"
    )
    @click.option(
        "--prefixes",
        "-p",
        type=click.STRING,
        multiple=True,
        help="Display stream of specific Prefixes.",
    )
    @click.pass_obj
    def snoop(
        cli_opts: Bunch,  # noqa: B902
        duration: int,
        initial_dump: bool,
        prefixes: List[str],
    ):
        """Snoop on fib streaming updates."""

        fib.FibSnoopCmd(cli_opts).run(duration, initial_dump, prefixes)


class StreamSummaryCli(object):
    @click.command()
    @click.pass_obj
    def _stream_summary(cli_opts):  # noqa: B902
        """Show basic info on all FIB subscribers"""
        cli_options = {}
        fib.StreamSummaryCmd(cli_opts).run(cli_options)
