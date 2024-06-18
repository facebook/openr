#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

import click
from bunch import Bunch
from openr.cli.commands import openr
from openr.py.openr.cli.clis.baseGroup import deduceCommandGroup
from openr.py.openr.cli.clis.config import ConfigShowCli
from openr.py.openr.cli.clis.decision import (
    DecisionAdjCli,
    DecisionRibPolicyCli,
    ReceivedRoutesCli,
)
from openr.py.openr.cli.clis.fib import FibMplsRoutesCli, FibUnicastRoutesCli
from openr.py.openr.cli.clis.kvstore import PeersCli, SummaryCli
from openr.py.openr.cli.clis.lm import LMLinksCli
from openr.py.openr.cli.clis.prefix_mgr import AdvertisedRoutesCli, OriginatedRoutesCli
from openr.py.openr.cli.utils.options import breeze_option


class OpenrCli:
    def __init__(self) -> None:
        # prefixmgr
        self.openr.add_command(OriginatedRoutesCli().show, name="originated-routes")
        self.openr.add_command(AdvertisedRoutesCli().show, name="advertised-routes")
        # decision
        self.openr.add_command(DecisionRibPolicyCli().show, name="rib-policy")
        self.openr.add_command(DecisionAdjCli().adj, name="neighbors")
        self.openr.add_command(ReceivedRoutesCli().show, name="table")
        # fib
        self.openr.add_command(FibMplsRoutesCli().routes, name="mpls-routes")
        self.openr.add_command(FibUnicastRoutesCli().routes, name="unicast-routes")
        # lm
        self.openr.add_command(LMLinksCli().links, name="interfaces")
        # kvstore
        self.openr.add_command(SummaryCli().summary, name="summary")
        self.openr.add_command(PeersCli().peers, name="peers")
        # openr
        self.openr.add_command(VersionCli().version, name="version")
        self.openr.add_command(OpenrValidateCli().validate, name="validate")
        self.openr.add_command(ConfigShowCli().show, name="config")

    @click.group(cls=deduceCommandGroup)
    @breeze_option("--fib_agent_port", type=int, help="Fib thrift server port")
    @breeze_option("--client-id", type=int, help="FIB Client ID")
    @breeze_option("--area", type=str, help="area identifier")
    @click.pass_context
    def openr(
        ctx: click.Context, fib_agent_port: int, client_id: int, area: str
    ) -> None:  # noqa: B902
        """CLI tool to peek into Openr information."""
        pass


class VersionCli:
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def version(cli_opts: Bunch, json: bool) -> None:  # noqa: B902
        """
        Get OpenR version
        """

        openr.VersionCmd(cli_opts).run(json)


class OpenrValidateCli:
    @click.command()
    @click.option(
        "--suppress-error/--print-all-info",
        default=False,
        help="Avoid printing out extra error information",
    )
    @click.option(
        "--json/--no-json",
        default=False,
        help="Additionally outputs whether or not all checks passed for each module in JSON format",
    )
    @click.pass_obj
    def validate(cli_opts: Bunch, suppress_error: bool, json: bool) -> None:
        """Run validation checks for all modules"""

        openr.OpenrValidateCmd(cli_opts).run(suppress_error, json)
