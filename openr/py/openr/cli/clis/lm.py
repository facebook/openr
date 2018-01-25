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

from openr.cli.commands import lm


class LMCli(object):

    def __init__(self):

        self.lm.add_command(LMLinksCli().links)
        self.lm.add_command(SetNodeOverloadCli().set_node_overload,
                            name='set-node-overload')
        self.lm.add_command(UnsetNodeOverloadCli().unset_node_overload,
                            name='unset-node-overload')
        self.lm.add_command(SetLinkOverloadCli().set_link_overload,
                            name='set-link-overload')
        self.lm.add_command(UnsetLinkOverloadCli().unset_link_overload,
                            name='unset-link-overload')
        self.lm.add_command(SetLinkMetricCli().set_link_metric,
                            name='set-link-metric')
        self.lm.add_command(UnsetLinkMetricCli().unset_link_metric,
                            name='unset-link-metric')

    @click.group()
    @click.option('--lm_cmd_port', default=None, help='Link Monitor port')
    @click.pass_context
    def lm(ctx, lm_cmd_port):  # noqa: B902
        ''' CLI tool to peek into Link Monitor module. '''

        if lm_cmd_port:
            ctx.obj.lm_cmd_port = lm_cmd_port


class LMLinksCli(object):

    @click.command()
    @click.option('--all/--no-all', default=False,
                  help='Show all links including ones without addresses')
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def links(cli_opts, all, json):  # noqa: B902
        ''' Dump all known links of the current host '''

        lm.LMLinksCmd(cli_opts).run(all, json)


class SetNodeOverloadCli(object):

    @click.command()
    @click.pass_obj
    def set_node_overload(cli_opts):  # noqa: B902
        ''' Set overload bit to stop transit traffic through node. '''

        lm.SetNodeOverloadCmd(cli_opts).run()


class UnsetNodeOverloadCli(object):

    @click.command()
    @click.pass_obj
    def unset_node_overload(cli_opts):  # noqa: B902
        ''' Unset overload bit to resume transit traffic through node. '''

        lm.UnsetNodeOverloadCmd(cli_opts).run()


class SetLinkOverloadCli(object):

    @click.command()
    @click.argument('interface')
    @click.pass_obj
    def set_link_overload(cli_opts, interface):  # noqa: B902
        ''' Set overload bit for a link. Transit traffic will be drained. '''

        lm.SetLinkOverloadCmd(cli_opts).run(interface)


class UnsetLinkOverloadCli(object):

    @click.command()
    @click.argument('interface')
    @click.pass_obj
    def unset_link_overload(cli_opts, interface):  # noqa: B902
        ''' Unset overload bit for a link to allow transit traffic. '''

        lm.UnsetLinkOverloadCmd(cli_opts).run(interface)


class SetLinkMetricCli(object):

    @click.command()
    @click.argument('interface')
    @click.argument('metric')
    @click.pass_obj
    def set_link_metric(cli_opts, interface, metric):  # noqa: B902
        '''
        Set custom metric value for a link. You can use high link metric value
        to emulate soft-drain behaviour.
        '''

        lm.SetLinkMetricCmd(cli_opts).run(interface, metric)


class UnsetLinkMetricCli(object):

    @click.command()
    @click.argument('interface')
    @click.pass_obj
    def unset_link_metric(cli_opts, interface):  # noqa: B902
        '''
        Unset previously set custom metric value on the interface.
        '''

        lm.UnsetLinkMetricCmd(cli_opts).run(interface)
