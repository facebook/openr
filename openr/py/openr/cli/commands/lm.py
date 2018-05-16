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

from openr.clients import lm_client
from openr.cli.utils import utils
from openr.utils import ipnetwork, printing

import click
import sys


class LMCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Link Monitor client '''

        self.client = lm_client.LMClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.lm_cmd_port),
            cli_opts.timeout,
            cli_opts.proto_factory)
        self.enable_color = cli_opts.enable_color


class SetNodeOverloadCmd(LMCmd):
    def run(self, yes=False):

        set_unset_overload(self.client, True, yes)


class UnsetNodeOverloadCmd(LMCmd):
    def run(self, yes=False):

        set_unset_overload(self.client, False, yes)


class SetLinkOverloadCmd(LMCmd):
    def run(self, interface, yes):

        set_unset_link_overload(self.client, True, interface, yes)


class UnsetLinkOverloadCmd(LMCmd):
    def run(self, interface, yes):

        set_unset_link_overload(self.client, False, interface, yes)


class SetLinkMetricCmd(LMCmd):
    def run(self, interface, metric, yes):

        set_unset_link_metric(self.client, True, interface, metric, yes)


class UnsetLinkMetricCmd(LMCmd):
    def run(self, interface, yes):

        set_unset_link_metric(self.client, False, interface, 0, yes)


class SetAdjMetricCmd(LMCmd):
    def run(self, node, interface, metric, yes):

        set_unset_adj_metric(self.client, True, node, interface, metric, yes)


class UnsetAdjMetricCmd(LMCmd):
    def run(self, node, interface, yes):

        set_unset_adj_metric(self.client, False, node, interface, 0, yes)


class VersionCmd(LMCmd):
    def run(self, json):
        openr_version = self.client.get_openr_version()

        if json:
            version = utils.thrift_to_dict(openr_version)
            print(utils.json_dumps(version))
        else:
            rows = []
            rows.append(['Current Version', ':', openr_version.version])
            rows.append(['Lowest Supported Version', ':',
                         openr_version.lowestSupportedVersion])
            print(printing.render_horizontal_table(
                rows, column_labels=[], tablefmt='plain'))


class BuildInfoCmd(LMCmd):
    def run(self, json):
        info = self.client.get_build_info()

        if json:
            info = utils.thrift_to_dict(info)
            print(utils.json_dumps(info))
        else:
            print('Build Information')
            print('  Built by: {}'.format(info.buildUser))
            print('  Built on: {}'.format(info.buildTime))
            print('  Built at: {}'.format(info.buildHost))
            print('  Build path: {}'.format(info.buildPath))
            print('  Package Name: {}'.format(info.buildPackageName))
            print('  Package Version: {}'.format(info.buildPackageVersion))
            print('  Package Release: {}'.format(info.buildPackageRelease))
            print('  Build Revision: {}'.format(info.buildRevision))
            print('  Build Upstream Revision: {}'
                  .format(info.buildUpstreamRevision))
            print('  Build Platform: {}'.format(info.buildPlatform))
            print('  Build Rule: {} ({}, {}, {})'.format(
                info.buildRule, info.buildType, info.buildTool, info.buildMode,
            ))


class LMLinksCmd(LMCmd):
    def run(self, all, json):
        links = self.client.dump_links(all)
        if json:
            self.print_links_json(links)
        else:
            if self.enable_color:
                overload_color = 'red' if links.isOverloaded else 'green'
                overload_status = click.style(
                    '{}'.format('YES' if links.isOverloaded else 'NO'),
                    fg=overload_color,
                )
                caption = 'Node Overload: {}'.format(overload_status)
                self.print_links_table(links.interfaceDetails, caption)
            else:
                caption = 'Node Overload: {}'.format(
                    'YES' if links.isOverloaded else 'NO'
                )
                self.print_links_table(links.interfaceDetails, caption)

    def interface_info_to_dict(self, interface_info):

        def _update(interface_info_dict, interface_info):
            interface_info_dict.update({
                # TO BE DEPRECATED SOON
                'v4Addrs': [ipnetwork.sprint_addr(v4Addr.addr)
                            for v4Addr in interface_info.v4Addrs],
                # TO BE DEPRECATED SOON
                'v6LinkLocalAddrs': [ipnetwork.sprint_addr(v6Addr.addr)
                                     for v6Addr in interface_info.v6LinkLocalAddrs],
                'networks': [ipnetwork.sprint_prefix(prefix)
                            for prefix in interface_info.networks],
            })

        return utils.thrift_to_dict(interface_info, _update)

    def interface_details_to_dict(self, interface_details):

        def _update(interface_details_dict, interface_details):
            interface_details_dict.update({
                'info': self.interface_info_to_dict(interface_details.info),
            })

        return utils.thrift_to_dict(interface_details, _update)

    def links_to_dict(self, links):

        def _update(links_dict, links):
            links_dict.update({
                'interfaceDetails': {k: self.interface_details_to_dict(v)
                                     for k, v in links.interfaceDetails.items()}
            })
            del links_dict['thisNodeName']

        return utils.thrift_to_dict(links, _update)

    def print_links_json(self, links):

        links_dict = {links.thisNodeName: self.links_to_dict(links)}
        print(utils.json_dumps(links_dict))

    @staticmethod
    def print_links_table(interfaces, caption=None):
        '''
        @param interfaces: dict<interface-name, InterfaceDetail>
        @param caption: Caption to show on table name
        '''

        rows = []
        columns = ['Interface', 'Status', 'Metric Override',
                   'Addresses']

        for (k, v) in sorted(interfaces.items()):
            state = 'Up' if v.info.isUp else click.style('Down', fg='red')
            metric_override = v.metricOverride if v.metricOverride else ''
            if v.isOverloaded:
                metric_override = click.style('Overloaded', fg='red')
            rows.append([k, state, metric_override, ''])
            firstAddr = True
            for prefix in (v.info.networks):
                addrStr = ipnetwork.sprint_addr(prefix.prefixAddress.addr)
                if firstAddr:
                    rows[-1][3] = addrStr
                    firstAddr = False
                else:
                    rows.append(['', '', '', addrStr])

        print(printing.render_horizontal_table(rows, columns, caption))
        print()


def set_unset_overload(client, overload, yes):
    '''
    Set/Unset overload bit for the node. Setting overload bit will take
    away all transit traffic going through node while node will still
    remains to be reachable.
    '''

    links = client.dump_links()
    host = links.thisNodeName
    print()

    if overload and links.isOverloaded:
        print('Node {} is already overloaded.\n'.format(host))
        sys.exit(0)

    if not overload and not links.isOverloaded:
        print('Node {} is not overloaded.\n'.format(host))
        sys.exit(0)

    action = 'set overload bit' if overload else 'unset overload bit'
    if not utils.yesno('Are you sure to {} for node {} ?'.format(action, host), yes):
        print()
        return

    links = client.set_unset_overload(overload)

    # Verify post command action
    if overload == links.isOverloaded:
        print('Successfully {}..\n'.format(action))
    else:
        print('Failed to {}.\n'.format(action))


def set_unset_link_overload(client, overload, interface, yes):
    '''
    Set/Unset link overload. All transit traffic on this link will be drained.
    Equivalent to hard draining the link
    '''

    links = client.dump_links()
    print()

    def intf_is_overloaded(links, interface):
        if interface in links.interfaceDetails:
            return links.interfaceDetails[interface].isOverloaded
        return False

    if overload and intf_is_overloaded(links, interface):
        print('Interface is already overloaded.\n')
        sys.exit(0)

    if not overload and not intf_is_overloaded(links, interface):
        print('Interface is not overloaded.\n')
        sys.exit(0)

    action = 'set overload bit' if overload else 'unset overload bit'
    question_str = 'Are you sure to {} for interface {} ?'
    if not utils.yesno(question_str.format(action, interface), yes):
        print()
        return

    links = client.set_unset_link_overload(overload, interface)

    if overload == intf_is_overloaded(links, interface):
        print('Successfully {} for the interface.\n'.format(action))
    else:
        print('Failed to {} for the interface.\n'.format(action))


def set_unset_link_metric(client, override, interface, metric, yes):
    '''
    Set/Unset metric override for the specific link. This can be used to
    emulate soft drains.
    '''

    links = client.dump_links()
    print()

    if interface not in links.interfaceDetails:
        print('No such interface: {}'.format(interface))
        return

    def intf_override(links, interface):
        if interface in links.interfaceDetails:
            return links.interfaceDetails[interface].metricOverride
        return None

    if not override and not intf_override(links, interface):
        print('Interface hasn\'t been assigned metric override.\n')
        sys.exit(0)

    action = 'set override metric' if override else 'unset override metric'
    question_str = 'Are you sure to {} for interface {} ?'
    if not utils.yesno(question_str.format(action, interface), yes):
        print()
        return

    links = client.set_unset_link_metric(override, interface, metric)

    # Verify post command action
    if override == (intf_override(links, interface) is not None):
        print('Successfully {} for the interface.\n'.format(action))
    else:
        print('Failed to {} for the interface.\n'.format(action))


def set_unset_adj_metric(client, override, node, interface, metric, yes):
    '''
    Set/Unset metric override for the specific adjacency.
    '''
    client.set_unset_adj_metric(override, node, interface, metric)
