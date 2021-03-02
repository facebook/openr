#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import click
from openr.cli.commands import config
from openr.cli.utils.options import breeze_option


class ConfigCli(object):
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
    def config(ctx):  # noqa: B902
        """ CLI tool to peek into Config Store module. """
        pass


class ConfigShowCli(object):
    @click.command()
    @click.pass_obj
    def show(cli_opts):  # noqa: B902
        """ Show openr running config """

        config.ConfigShowCmd(cli_opts).run()


class ConfigDryRunCli(object):
    @click.command()
    @click.argument("file")
    @click.pass_obj
    def dryrun(cli_opts, file):  # noqa: B902
        """ Dryrun openr config, output parsed config upon success"""

        config.ConfigDryRunCmd(cli_opts).run(file)


class ConfigCompareCli(object):
    @click.command()
    @click.argument("file")
    @click.pass_obj
    def compare(cli_opts, file):  # noqa: B902
        """ Migration cli: Compare config with current running config """

        config.ConfigCompareCmd(cli_opts).run(file)


class ConfigPrefixAllocatorCli(object):
    @click.command()
    @click.pass_obj
    def config_prefix_allocator(cli_opts):  # noqa: B902
        """ Dump prefix allocation config """

        config.ConfigPrefixAllocatorCmd(cli_opts).run()


class ConfigLinkMonitorCli(object):
    @click.command()
    @click.pass_obj
    def config_link_monitor(cli_opts):  # noqa: B902
        """ Dump link monitor config """

        config.ConfigLinkMonitorCmd(cli_opts).run()


class ConfigPrefixManagerCli(object):
    @click.command()
    @click.pass_obj
    def config_prefix_manager(cli_opts):  # noqa: B902
        """ Dump prefix manager config """

        config.ConfigPrefixManagerCmd(cli_opts).run()


class ConfigEraseCli(object):
    @click.command()
    @click.argument("key")
    @click.pass_obj
    def config_erase(cli_opts, key):  # noqa: B902
        """ Erase a config key """

        config.ConfigEraseCmd(cli_opts).run(key)


class ConfigStoreCli(object):
    @click.command()
    @click.argument("key")
    @click.argument("value")
    @click.pass_obj
    def config_store(cli_opts, key, value):  # noqa: B902
        """ Store a config key """

        config.ConfigStoreCmd(cli_opts).run(key, value)
