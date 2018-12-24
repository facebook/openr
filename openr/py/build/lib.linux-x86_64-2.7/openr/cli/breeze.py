#!/usr/bin/env python

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

import click
import json

# Disable click unicode literals warning before importing other modules
click.disable_unicode_literals_warning = True

from openr.cli.clis import config, decision, fib, health_checker, kvstore, lm, \
    monitor, perf, prefix_mgr
from openr.utils.consts import Consts


class CliOptions(object):
    ''' Object for holding initial CLI state '''

    def __init__(self, host, timeout, ports_config, enable_color):
        self.hostname = host
        self.timeout = timeout
        self.ports_config = ports_config
        self.enable_color = enable_color

    def __repr__(self):
        ''' String representation for debugging '''

        return 'Host: {}, Timeout: {}\nPorts config:\n{}'.format(
            self.hostname, self.timeout, self.ports_config)


@click.group()
@click.option('--host', '-H', default='localhost',
              type=str, help='Host to connect to (default = localhost)')
@click.option('--timeout', '-t', default=Consts.TIMEOUT_MS,
              type=int, help='Timeout for socket communication in ms')
@click.option('--ports-config-file', '-f', default=None,
              type=str, help='JSON file for ports config')
@click.option('--color/--no-color', default=True, help='Enalbe coloring display')
@click.pass_context
def cli(ctx, host, timeout, ports_config_file, color):
    ''' Command line tools for Open/R. '''

    ports_config = {}
    if ports_config_file:
        with open(ports_config_file, 'r') as f:
            ports_config = json.load(f)

    ctx.obj = CliOptions(host, timeout, ports_config, color)


def main():
    ''' entry point for breeze '''

    # add cli submodules
    cli.add_command(config.ConfigCli().config)
    cli.add_command(decision.DecisionCli().decision)
    cli.add_command(fib.FibCli().fib)
    cli.add_command(health_checker.HealthCheckerCli().healthchecker)
    cli.add_command(kvstore.KvStoreCli().kvstore)
    cli.add_command(lm.LMCli().lm)
    cli.add_command(monitor.MonitorCli().monitor)
    cli.add_command(perf.PerfCli().perf)
    cli.add_command(prefix_mgr.PrefixMgrCli().prefixmgr)

    # let the magic begin
    cli()


if __name__ == '__main__':
    main()
