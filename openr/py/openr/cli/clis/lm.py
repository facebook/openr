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

from openr.cli.commands import lm
from openr.cli.commands import kvstore
from openr.cli.utils.utils import parse_nodes
from openr.cli.utils import utils


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
        self.lm.add_command(SetAdjMetricCli().set_adj_metric,
                            name='set-adj-metric')
        self.lm.add_command(UnsetAdjMetricCli().unset_adj_metric,
                            name='unset-adj-metric')
        self.lm.add_command(VersionCli().version, name='version')
        self.lm.add_command(BuildInfoCli().build_info, name='build-info')

    @click.group()
    @click.option('--lm_cmd_port', default=None, type=int, help='Link Monitor port')
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
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def set_node_overload(cli_opts, yes):  # noqa: B902
        ''' Set overload bit to stop transit traffic through node. '''

        lm.SetNodeOverloadCmd(cli_opts).run(yes)


class UnsetNodeOverloadCli(object):

    @click.command()
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def unset_node_overload(cli_opts, yes):  # noqa: B902
        ''' Unset overload bit to resume transit traffic through node. '''

        lm.UnsetNodeOverloadCmd(cli_opts).run(yes)


class SetLinkOverloadCli(object):

    @click.command()
    @click.argument('interface')
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def set_link_overload(cli_opts, interface, yes):  # noqa: B902
        ''' Set overload bit for a link. Transit traffic will be drained. '''

        lm.SetLinkOverloadCmd(cli_opts).run(interface, yes)


class UnsetLinkOverloadCli(object):

    @click.command()
    @click.argument('interface')
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def unset_link_overload(cli_opts, interface, yes):  # noqa: B902
        ''' Unset overload bit for a link to allow transit traffic. '''

        lm.UnsetLinkOverloadCmd(cli_opts).run(interface, yes)


class SetLinkMetricCli(object):

    @click.command()
    @click.argument('interface')
    @click.argument('metric')
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def set_link_metric(cli_opts, interface, metric, yes):  # noqa: B902
        '''
        Set custom metric value for a link. You can use high link metric value
        to emulate soft-drain behaviour.
        '''

        lm.SetLinkMetricCmd(cli_opts).run(interface, metric, yes)


class UnsetLinkMetricCli(object):

    @click.command()
    @click.argument('interface')
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def unset_link_metric(cli_opts, interface, yes):  # noqa: B902
        '''
        Unset previously set custom metric value on the interface.
        '''

        lm.UnsetLinkMetricCmd(cli_opts).run(interface, yes)


class SetAdjMetricCli(object):

    @click.command()
    @click.argument('node')
    @click.argument('interface')
    @click.argument('metric')
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def set_adj_metric(cli_opts, node, interface, metric, yes):  # noqa: B902
        '''
        Set custom metric value for the adjacency
        '''
        question_str = 'Are you sure to override metric '\
                       'for adjacency {} {} ?'.format(node, interface)
        if not utils.yesno(question_str, yes):
            return

        lm.SetAdjMetricCmd(cli_opts).run(node, interface, metric, yes)
        nodes = parse_nodes(cli_opts.host, '', cli_opts.lm_cmd_port)
        kvstore.ShowAdjNodeCmd(cli_opts).run(nodes, node, interface)


class UnsetAdjMetricCli(object):

    @click.command()
    @click.argument('node')
    @click.argument('interface')
    @click.option('--yes', is_flag=True, help='Make command non-interactive')
    @click.pass_obj
    def unset_adj_metric(cli_opts, node, interface, yes):  # noqa: B902
        '''
        Unset previously set custom metric value on the node.
        '''
        question_str = 'Are you sure to unset metric ' \
                       'for adjacency {} {} ?'.format(node, interface)
        if not utils.yesno(question_str, yes):
            return

        lm.UnsetAdjMetricCmd(cli_opts).run(node, interface, yes)
        nodes = parse_nodes(cli_opts.host, '', cli_opts.lm_cmd_port)
        kvstore.ShowAdjNodeCmd(cli_opts).run(nodes, node, interface)


class VersionCli(object):

    @click.command()
    @click.option('--json/--no-json', default=False, help='Dump in JSON format')
    @click.pass_obj
    def version(cli_opts, json):  # noqa: B902
        '''
        Get OpenR version
        '''

        lm.VersionCmd(cli_opts).run(json)


class BuildInfoCli(object):

    @click.command()
    @click.option('--json/--no-json', default=False, help='Dump in JSON format')
    @click.pass_obj
    def build_info(cli_opts, json):  # noqa: B902
        '''
        Get build information from running version of Open/R
        '''

        lm.BuildInfoCmd(cli_opts).run(json)
