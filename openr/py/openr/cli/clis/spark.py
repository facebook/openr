#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import click
from openr.py.openr.cli.clis.baseGroup import deduceCommandGroup
from openr.py.openr.cli.commands import spark


class SparkCli:
    def __init__(self):
        self.spark.add_command(SparkGRCli().graceful_restart, name="graceful-restart")
        self.spark.add_command(SparkNeighborCli().neighbors, name="neighbors")
        self.spark.add_command(SparkValidateCli().validate, name="validate")

    @click.group(cls=deduceCommandGroup)
    @click.pass_context
    def spark(ctx):  # noqa: B902
        """CLI tool to peek into Spark information."""
        pass


class SparkValidateCli:
    @click.command()
    @click.option(
        "--detail/--no-detail",
        default=False,
        help="Verbose mode outputs information about all neighbors regardless of their state.",
    )
    @click.pass_obj
    def validate(cli_opts, detail):
        """Outputs number of neighbors in ESTABLISHED state and information about
        neighbors which are not"""

        spark.ValidateCmd(cli_opts).run(detail)


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
