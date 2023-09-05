# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import click
from openr.cli.clis.baseGroup import deduceCommandGroup
from openr.cli.commands import dispatcher


class DispatcherCli:
    def __init__(self):
        self.dispatcher.add_command(FiltersCli().filters)
        self.dispatcher.add_command(QueuesCli().queues)

    @click.group(cls=deduceCommandGroup)
    @click.pass_context
    def dispatcher(ctx):  # noqa: B902
        """CLI tool to peek into Dispatcher module."""
        pass


class FiltersCli:
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def filters(cli_opts, json):  # noqa: B902
        dispatcher.FiltersCmd(cli_opts).run(json)


class QueuesCli:
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def queues(cli_opts, json):  # noqa: B902
        dispatcher.QueuesCmd(cli_opts).run(json)
