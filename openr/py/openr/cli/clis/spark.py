#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import click
from openr.cli.commands import spark


class SparkCli(object):
    def __init__(self):
        self.spark.add_command(SparkGRCli().graceful_restart, name="graceful-restart")

    @click.group()
    @click.pass_context
    def spark(ctx):  # noqa: B902
        """ CLI tool to peek into Spark information. """
        pass


class SparkGRCli(object):
    @click.command()
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.pass_obj
    def graceful_restart(cli_opts, yes):  # noqa: B902
        """Force to send out restarting msg indicating GR"""

        spark.GracefulRestartCmd(cli_opts).run(yes)
