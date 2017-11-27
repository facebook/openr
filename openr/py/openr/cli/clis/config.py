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
import zmq

from openr.cli.commands import config
from openr.utils.consts import Consts
from openr.cli.utils import utils


class ConfigContext(object):
    def __init__(self, verbose, zmq_ctx, timeout, config_store_url):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
        '''

        self.verbose = verbose
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx
        self.config_store_url = config_store_url
        self.proto_factory = Consts.PROTO_FACTORY


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
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def config(ctx, config_store_url, verbose):  # noqa: B902
        ''' CLI tool to peek into Config Store module. '''

        config_store_url = config_store_url or "{}_{}".format(
            Consts.CONFIG_STORE_URL_PREFIX,
            utils.get_connected_node_name(
                ctx.obj.hostname,
                ctx.obj.ports_config.get('lm_cmd_port', None) or
                Consts.LINK_MONITOR_CMD_PORT))

        override_url = ctx.obj.ports_config.get('config_store_url', None)
        ctx.obj = ConfigContext(
            verbose, zmq.Context(),
            ctx.obj.timeout,
            override_url or config_store_url)


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
