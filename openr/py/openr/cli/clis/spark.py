#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import click
from openr.cli.commands import spark


class SparkCli:
    def __init__(self):
        self.spark.add_command(SparkGRCli().graceful_restart, name="graceful-restart")
        self.spark.add_command(SparkNeighborCli().neighbors, name="neighbors")

    @click.group()
    @click.pass_context
    def spark(ctx):  # noqa: B902
        """CLI tool to peek into Spark information."""
        pass


class SparkNeighborCli:
    @click.command()
    @click.option(
        "--detail/--no-detail",
        default=False,
        help="Show all details including timers etc.",
    )
    @click.option("--json/--no-json", default=False, help="Output in JSON format")
    @click.pass_obj
    def neighbors(cli_opts, detail, json):  # noqa: B902
        """Dump all detected neighbor information"""

        spark.NeighborCmd(cli_opts).run(json, detail)


class SparkGRCli:
    @click.command()
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.pass_obj
    def graceful_restart(cli_opts, yes):  # noqa: B902
        """Force to send out restarting msg indicating GR"""

        spark.GracefulRestartCmd(cli_opts).run(yes)
