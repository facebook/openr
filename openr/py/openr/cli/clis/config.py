#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import click
from bunch import Bunch
from openr.cli.commands import config


class ConfigCli:
    def __init__(self):
        self.config.add_command(ConfigShowCli().show, name="show")
        self.config.add_command(ConfigDryRunCli().dryrun, name="dryrun")
        self.config.add_command(ConfigCompareCli().compare, name="compare")
        self.config.add_command(
            ConfigPrefixAllocatorCli().config_prefix_allocator,
            name="prefix-allocator-config",
        )
        self.config.add_command(
            ConfigLinkMonitorCli().config_link_monitor, name="link-monitor-config"
        )
        self.config.add_command(
            ConfigPrefixManagerCli().config_prefix_manager, name="prefix-manager-config"
        )
        self.config.add_command(ConfigEraseCli().config_erase, name="erase")
        self.config.add_command(ConfigStoreCli().config_store, name="store")

    @click.group()
    @click.pass_context
    def config(ctx: Bunch) -> None:  # noqa: B902
        """CLI tool to peek into Config Store module."""
        pass


class ConfigShowCli:
    @click.command()
    @click.pass_obj
    def show(cli_opts: Bunch) -> None:  # noqa: B902
        """Show openr running config"""

        config.ConfigShowCmd(cli_opts).run()


class ConfigDryRunCli:
    @click.command()
    @click.argument("file")
    @click.pass_obj
    @click.pass_context
    def dryrun(ctx: click.Context, cli_opts: Bunch, file: str) -> None:  # noqa: B902
        """Dryrun/validate openr config, output JSON parsed config upon success"""

        config.ConfigDryRunCmd(cli_opts).run(file)
        # TODO(@cooper): Fix emulation to handle UNIX return codes
        # neteng/emulation/emulator/testing/openr/test_breeze.py expects all to return 0
        # This is incorrect and needs to be fixed
        # ret_val = config.ConfigDryRunCmd(cli_opts).run(file)
        # ctx.exit(ret_val if ret_val else 0)


class ConfigCompareCli:
    @click.command()
    @click.argument("file")
    @click.pass_obj
    def compare(cli_opts: Bunch, file: str) -> None:  # noqa: B902
        """Migration cli: Compare config with current running config"""

        config.ConfigCompareCmd(cli_opts).run(file)


class ConfigPrefixAllocatorCli:
    @click.command()
    @click.pass_obj
    def config_prefix_allocator(cli_opts):  # noqa: B902
        """Dump prefix allocation config"""

        config.ConfigPrefixAllocatorCmd(cli_opts).run()


class ConfigLinkMonitorCli:
    @click.command()
    @click.pass_obj
    def config_link_monitor(cli_opts):  # noqa: B902
        """Dump link monitor config"""

        config.ConfigLinkMonitorCmd(cli_opts).run()


class ConfigPrefixManagerCli:
    @click.command()
    @click.pass_obj
    def config_prefix_manager(cli_opts):  # noqa: B902
        """Dump prefix manager config"""

        config.ConfigPrefixManagerCmd(cli_opts).run()


class ConfigEraseCli:
    @click.command()
    @click.argument("key")
    @click.pass_obj
    def config_erase(cli_opts, key):  # noqa: B902
        """Erase a config key"""

        config.ConfigEraseCmd(cli_opts).run(key)


class ConfigStoreCli:
    @click.command()
    @click.argument("key")
    @click.argument("value")
    @click.pass_obj
    def config_store(cli_opts, key, value):  # noqa: B902
        """Store a config key"""

        config.ConfigStoreCmd(cli_opts).run(key, value)
