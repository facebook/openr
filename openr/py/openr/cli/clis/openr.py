#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import click
from openr.cli.commands import openr


class OpenrCli(object):
    def __init__(self):
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
