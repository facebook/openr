#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import click
from openr.py.openr.cli.commands import perf
from openr.py.openr.cli.utils.options import breeze_option


class PerfCli:
    def __init__(self):
        self.perf.add_command(ViewFibCli().fib)

    @click.group()
    @click.pass_context
    def perf(ctx):  # noqa: B902
        """CLI tool to view latest perf log of each module."""
        pass


class ViewFibCli:
    @click.command()
    @click.pass_obj
    def fib(cli_opts):  # noqa: B902
        """View latest perf log of fib module from this node"""

        perf.ViewFibCmd(cli_opts).run()
