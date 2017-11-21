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

from openr.clients import lm_client
from openr.cli.utils import utils
from openr.utils import printing

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
    def run(self):

        set_unset_overload(self.client, True)


class UnsetNodeOverloadCmd(LMCmd):
    def run(self):

        set_unset_overload(self.client, False)


class SetLinkOverloadCmd(LMCmd):
    def run(self, interface):

        set_unset_link_overload(self.client, True, interface)


class UnsetLinkOverloadCmd(LMCmd):
    def run(self, interface):

        set_unset_link_overload(self.client, False, interface)


class SetLinkMetricCmd(LMCmd):
    def run(self, interface, metric):

        set_unset_link_metric(self.client, True, interface, metric)


class UnsetLinkMetricCmd(LMCmd):
    def run(self, interface):

        set_unset_link_metric(self.client, False, interface, 0)


class LMLinksCmd(LMCmd):
    def run(self, all, json):
        links = self.client.dump_links(all)
        if json:
            self.print_links_json(links)
        else:
            if self.enable_color:
                overload_color = 'red' if links.isOverloaded else 'green'
                overload_status = click.style('{}'.format('YES' if links.isOverloaded else 'NO'),
                                              fg=overload_color)
                caption = 'Node Overload: {}'.format(overload_status)
                self.print_links_table(links.interfaceDetails, caption)
            else:
                caption = 'Node Overload: {}'.format('YES' if links.isOverloaded else 'NO')
                self.print_links_table(links.interfaceDetails, caption)

    def interface_info_to_dict(self, interface_info):

        def _update(interface_info_dict, interface_info):
            interface_info_dict.update({
                'v4Addrs': [utils.sprint_addr(v4Addr.addr)
                            for v4Addr in interface_info.v4Addrs],
                'v6LinkLocalAddrs': [utils.sprint_addr(v6Addr.addr)
                                     for v6Addr in interface_info.v6LinkLocalAddrs]
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
        columns = ['Interface', 'Status', 'Overloaded', 'Metric Override',
                   'ifIndex', 'Addresses']

        for (k, v) in sorted(interfaces.items()):
            state = 'Up' if v.info.isUp else 'Down'
            overloaded = 'True' if v.isOverloaded else ''
            metric_override = v.metricOverride if v.metricOverride else ''
            index = v.info.ifIndex
            rows.append([k, state, overloaded, metric_override, index, ''])
            firstAddr = True
            for a in (v.info.v4Addrs + v.info.v6LinkLocalAddrs):
                addrStr = utils.sprint_addr(a.addr)
                if firstAddr:
                    rows[-1][5] = addrStr
                    firstAddr = False
                else:
                    rows.append(['', '', '', '', '', addrStr])

        print(printing.render_horizontal_table(rows, columns, caption))
        print()


def set_unset_overload(client, overload):
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
    if not utils.yesno('Are you sure to {} for node {} ?'.format(action, host)):
        print()
        return

    links = client.set_unset_overload(overload)

    # Verify post command action
    if overload == links.isOverloaded:
        print('Successfully {}..\n'.format(action))
    else:
        print('Failed to {}.\n'.format(action))


def set_unset_link_overload(client, overload, interface):
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
    if not utils.yesno(question_str.format(action, interface)):
        print()
        return

    links = client.set_unset_link_overload(overload, interface)

    if overload == intf_is_overloaded(links, interface):
        print('Successfully {} for the interface.\n'.format(action))
    else:
        print('Failed to {} for the interface.\n'.format(action))


def set_unset_link_metric(client, override, interface, metric):
    '''
    Set/Unset metric override for the specific link. This can be used to
    emulate soft drains.
    '''

    links = client.dump_links()
    print()

    def intf_override(links, interface):
        if interface in links.interfaceDetails:
            return links.interfaceDetails[interface].metricOverride
        return None

    if not override and not intf_override(links, interface):
        print('Interface hasn\'t been assigned metric override.\n')
        sys.exit(0)

    action = 'set override metric' if override else 'unset override metric'
    question_str = 'Are you sure to {} for interface {} ?'
    if not utils.yesno(question_str.format(action, interface)):
        print()
        return

    links = client.set_unset_link_metric(override, interface, metric)

    # Verify post command action
    if override == (intf_override(links, interface) is not None):
        print('Successfully {} for the interface.\n'.format(action))
    else:
        print('Failed to {} for the interface.\n'.format(action))
