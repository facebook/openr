#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import click
from fastcli.click import inject_fastcli
from openr.cli.clis import (
    baseGroup,
    config,
    decision,
    dispatcher,
    fib,
    kvstore,
    lm,
    monitor,
    openr,
    perf,
    prefix_mgr,
    spark,
    tech_support,
)
from openr.py.openr.cli.utils.options import breeze_option, OPTIONS, str2cert
from thrift.Thrift import TApplicationException
from thrift.transport.TTransport import TTransportException


# Plugin module is optional
plugin = None
try:
    from openr.cli.clis import plugin
except ImportError:
    pass


@click.group(name="breeze", cls=baseGroup.deduceCommandGroup)
# make host eager (option callback is called before others) sice some default
# options can depend on this
@breeze_option("--host", "-H", help="Host to connect to", is_eager=True)
@breeze_option(
    "--timeout", "-t", type=click.INT, help="Timeout for socket communication in ms"
)
@breeze_option("--ssl/--no-ssl", help="Prefer SSL thrift to connect to OpenR")
@breeze_option(
    "--cert-reqs",
    type=click.Choice(["none", "optional", "required"], case_sensitive=False),
    callback=str2cert,
    help="If we are connecting to an SSL server, this indicates whether to "
    "verify peer certificate",
)
@breeze_option(
    "--cert-file",
    help="If we are connecting to an SSL server, this points at the "
    "certfile we will present",
)
@breeze_option(
    "--key-file",
    help="If we are connecting to an SSL server, this points at the "
    "keyfile associated with the certificate will present",
)
@breeze_option(
    "--ca-file",
    help="If we are connecting to an SSL server, this points at the "
    "certificate authority we will use to verify peers",
)
@breeze_option(
    "--acceptable-peer-name",
    help="If we are connecting to an SSL server, this is the common "
    "name we deem acceptable to connect to.",
)
@click.pass_context
def cli(ctx, *args, **kwargs) -> None:
    """Command line tools for Open/R."""

    # Default config options
    ctx.obj = OPTIONS


def get_breeze_cli():
    # add cli submodules
    cli.add_command(config.ConfigCli().config)
    cli.add_command(decision.DecisionCli().decision)
    cli.add_command(dispatcher.DispatcherCli().dispatcher)
    cli.add_command(fib.FibCli().fib)
    cli.add_command(kvstore.KvStoreCli().kvstore)
    cli.add_command(lm.LMCli().lm)
    cli.add_command(monitor.MonitorCli().monitor)
    cli.add_command(openr.OpenrCli().openr)
    cli.add_command(perf.PerfCli().perf)
    cli.add_command(prefix_mgr.PrefixMgrCli().prefixmgr)
    cli.add_command(spark.SparkCli().spark)
    cli.add_command(tech_support.TechSupportCli().tech_support)
    if plugin:
        plugin.plugin_start(cli)

    return cli


def main() -> None:
    """entry point for breeze"""

    # attach CLI commands
    main_cli = get_breeze_cli()
    inject_fastcli(main_cli)

    try:
        main_cli()
    except TApplicationException as e:
        raise SystemExit("Thrift Application Exception: {}".format(str(e)))
    except TTransportException as e:
        raise SystemExit("Failed connecting to host: {}".format(str(e)))
    except Exception as e:
        raise SystemExit("Failed with exception: {}".format(str(e)))


if __name__ == "__main__":
    main()
