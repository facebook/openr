#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import
from __future__ import print_function
from __future__ import unicode_literals
from __future__ import division
from builtins import object

import click

from openr.cli.commands import health_checker


class HealthCheckerCli(object):
    def __init__(self):
        self.healthchecker.add_command(PeekCli().peek)

    @click.group()
    @click.option('--health_checker_cmd_port', default=None, type=int,
                  help='Health Checker port')
    @click.pass_context
    def healthchecker(ctx, health_checker_cmd_port):  # noqa: B902
        ''' CLI tool to peek into Health Checker module. '''

        if health_checker_cmd_port:
            ctx.obj.health_checker_cmd_port = health_checker_cmd_port


class PeekCli(object):

    @click.command()
    @click.pass_obj
    def peek(cli_opts):  # noqa: B902
        ''' View the health checker result from this node '''

        health_checker.PeekCmd(cli_opts).run()
