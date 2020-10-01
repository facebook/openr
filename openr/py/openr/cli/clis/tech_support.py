#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import click
from openr.cli.commands.tech_support import TechSupportCmd
from openr.cli.utils.options import breeze_option


class TechSupportCli(object):
    @click.command(name="tech-support")
    @breeze_option("--fib_agent_port", type=int, help="Fib thrift server port")
    @click.option(
        "--routes", is_flag=True, default=True, help="Show routes in techsupport output"
    )
    @click.pass_context
    def tech_support(ctx, fib_agent_port, routes):  # noqa: B902
        """ Extensive logging of Open/R's state for debugging """

        """
        - Counters (FIB) and recent log samples

        - Recent perf events
        """

        TechSupportCmd(ctx.obj).run(routes)
