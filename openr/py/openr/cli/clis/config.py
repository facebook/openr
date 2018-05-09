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

from openr.cli.commands import config


class ConfigCli(object):
    def __init__(self):
        self.config.add_command(ConfigPrefixAllocatorCli().config_prefix_allocator,
                                name='prefix-allocator-config')
        self.config.add_command(ConfigLinkMonitorCli().config_link_monitor,
                                name='link-monitor-config')
        self.config.add_command(ConfigPrefixManagerCli().config_prefix_manager,
                                name='prefix-manager-config')
        self.config.add_command(ConfigEraseCli().config_erase, name='erase')
        self.config.add_command(ConfigStoreCli().config_store, name='store')

    @click.group()
    @click.option('--config_store_url', default=None, help='Config Store IPC URL')
    @click.pass_context
    def config(ctx, config_store_url):  # noqa: B902
        ''' CLI tool to peek into Config Store module. '''

        if config_store_url:
            ctx.obj.config_store_url = config_store_url


class ConfigPrefixAllocatorCli(object):

    @click.command()
    @click.pass_obj
    def config_prefix_allocator(cli_opts):  # noqa: B902
        ''' Dump prefix allocation config '''

        config.ConfigPrefixAllocatorCmd(cli_opts).run()


class ConfigLinkMonitorCli(object):

    @click.command()
    @click.pass_obj
    def config_link_monitor(cli_opts):  # noqa: B902
        ''' Dump link monitor config '''

        config.ConfigLinkMonitorCmd(cli_opts).run()


class ConfigPrefixManagerCli(object):

    @click.command()
    @click.pass_obj
    def config_prefix_manager(cli_opts):  # noqa: B902
        ''' Dump link monitor config '''

        config.ConfigPrefixManagerCmd(cli_opts).run()


class ConfigEraseCli(object):

    @click.command()
    @click.argument('key')
    @click.pass_obj
    def config_erase(cli_opts, key):  # noqa: B902
        ''' Erase a config key '''

        config.ConfigEraseCmd(cli_opts).run(key)


class ConfigStoreCli(object):

    @click.command()
    @click.argument('key')
    @click.argument('value')
    @click.pass_obj
    def config_store(cli_opts, key, value):  # noqa: B902
        ''' Store a config key '''

        config.ConfigStoreCmd(cli_opts).run(key, value)
