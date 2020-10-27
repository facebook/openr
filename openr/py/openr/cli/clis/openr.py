#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import click
from openr.cli.clis.decision import DecisionRibPolicyCli, ReceivedRoutesCli
from openr.cli.clis.fib import FibMplsRoutesCli, FibUnicastRoutesCli
from openr.cli.clis.lm import LMAdjCli, LMLinksCli
from openr.cli.clis.prefix_mgr import AdvertisedRoutesCli
from openr.cli.commands import openr


class OpenrCli(object):
    def __init__(self):
        self.openr.add_command(AdvertisedRoutesCli().show, name="advertised-routes")
        self.openr.add_command(DecisionRibPolicyCli().show, name="rib-policy")
        self.openr.add_command(FibMplsRoutesCli().routes, name="mpls-routes")
        self.openr.add_command(FibUnicastRoutesCli().routes, name="unicast-routes")
        self.openr.add_command(LMAdjCli().adj, name="neighbors")
        self.openr.add_command(LMLinksCli().links, name="interfaces")
        self.openr.add_command(ReceivedRoutesCli().show, name="received-routes")
        self.openr.add_command(VersionCli().version, name="version")

    @click.group()
    @click.pass_context
    def openr(ctx):  # noqa: B902
        """ CLI tool to peek into Openr information. """
        pass


class VersionCli(object):
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def version(cli_opts, json):  # noqa: B902
        """
        Get OpenR version
        """

        openr.VersionCmd(cli_opts).run(json)
