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

import bunch
import click
import json
import zmq

# Disable click unicode literals warning before importing other modules
click.disable_unicode_literals_warning = True

from openr.Platform import ttypes as platform_types
from openr.cli.clis import config, decision, fib, health_checker, kvstore
from openr.cli.clis import lm, monitor, perf, prefix_mgr, tech_support
from openr.utils.consts import Consts


@click.group(name='breeze')
@click.option('--host', '-H', default='localhost',
              type=str, help='Host to connect to (default = localhost)')
@click.option('--timeout', '-t', default=Consts.TIMEOUT_MS,
              type=int, help='Timeout for socket communication in ms')
@click.option('--ports-config-file', '-f', default=None,
              type=str, help='JSON file for ports config')
@click.option('--color/--no-color', default=True,
              help='Enable coloring display')
@click.option('--verbose/--no-verbose', default=False,
              help='Print verbose information')
@click.pass_context
def cli(ctx, host, timeout, ports_config_file, color, verbose):
    ''' Command line tools for Open/R. '''

    # Default config options
    ctx.obj = bunch.Bunch({
        'client_id': platform_types.FibClient.OPENR,
        'config_store_url': Consts.CONFIG_STORE_URL,
        'decision_rep_port': Consts.DECISION_REP_PORT,
        'enable_color': color,
        'fib_agent_port': Consts.FIB_AGENT_PORT,
        'fib_rep_port': Consts.FIB_REP_PORT,
        'health_checker_cmd_port': Consts.HEALTH_CHECKER_CMD_PORT,
        'host': host,
        'kv_pub_port': Consts.KVSTORE_PUB_PORT,
        'kv_rep_port': Consts.KVSTORE_REP_PORT,
        'lm_cmd_port': Consts.LINK_MONITOR_CMD_PORT,
        'monitor_pub_port': Consts.MONITOR_PUB_PORT,
        'monitor_rep_port': Consts.MONITOR_REP_PORT,
        'prefix_mgr_cmd_port': Consts.PREFIX_MGR_CMD_PORT,
        'proto_factory': Consts.PROTO_FACTORY,
        'timeout': timeout,
        'verbose': verbose,
        'zmq_ctx': zmq.Context(),
    })

    # Get override port configs
    if ports_config_file:
        with open(ports_config_file, 'r') as f:
            override_ports_config = json.load(f)
            for key, value in override_ports_config.items():
                ctx.obj[key] = value


def get_breeze_cli():

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
    cli.add_command(tech_support.TechSupportCli().tech_support)
    return cli


def main():
    ''' entry point for breeze '''

    # let the magic begin
    cli = get_breeze_cli()
    cli()


if __name__ == '__main__':
    main()
