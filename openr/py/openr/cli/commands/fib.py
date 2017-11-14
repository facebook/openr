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

import sys
import zmq
import click

from openr.clients import fib_client
from openr.clients import decision_client
from openr.cli.utils import utils
from openr.utils import printing
from openr.IpPrefix import ttypes as ip_types
from openr.LinuxPlatform import LinuxFibService


def build_routes(prefixes, nexthops):
    '''
    :param prefixes: List of prefixes in string representation
    :param nexthops: List of nexthops ip addresses in string presentation

    :returns: list ip_types.UnicastRoute (structured routes)
    :rtype: list
    '''

    prefixes = [utils.ip_str_to_prefix(p) for p in prefixes]
    nhs = []
    for nh_iface in nexthops:
        iface, addr = None, None
        # Nexthop may or may not be link-local. Handle it here well
        if '@' in nh_iface:
            addr, iface = nh_iface.split('@')
        elif '%' in nh_iface:
            addr, iface = nh_iface.split('%')
        else:
            addr = nh_iface
        nexthop = utils.ip_str_to_addr(addr)
        nexthop.ifName = iface
        nhs.append(nexthop)
    return [ip_types.UnicastRoute(dest=p, nexthops=nhs) for p in prefixes]


def get_route_as_dict(routes):
    '''
    Convert a routeDb into a dict representing routes in str format

    :param routes: list ip_types.UnicastRoute (structured routes)

    :returns: dict of routes (prefix : [nexthops]
    :rtype: dict
    '''

    # Thrift object instances do not have hash support
    # Make custom stringified object so we can hash and diff
    # dict of prefixes(str) : nexthops(str)
    routes_dict = {utils.sprint_prefix(route.dest):
                   sorted([ip_nexthop_to_str(nh) for nh in route.nexthops])
                   for route in routes}

    return routes_dict


def routes_difference(lhs, rhs):
    '''
    Get routeDb delta between provided inputs

    :param lhs: list ip_types.UnicastRoute (structured routes)
    :param rhs: list ip_types.UnicastRoute (structured routes)

    :returns: list ip_types.UnicastRoute (structured routes)
    :rtype: list
    '''

    diff = []

    # dict of prefixes(str) : nexthops(str)
    _lhs = get_route_as_dict(lhs)
    _rhs = get_route_as_dict(rhs)

    diff_prefixes = set(_lhs) - set(_rhs)

    for prefix in diff_prefixes:
        diff.extend(build_routes([prefix], _lhs[prefix]))

    return diff


def prefixes_with_different_nexthops(lhs, rhs):
    '''
    Get prefixes common to both routeDbs with different nexthops

    :param lhs: list ip_types.UnicastRoute (structured routes)
    :param rhs: list ip_types.UnicastRoute (structured routes)

    :returns: list str of IpPrefix common to lhs and rhs but
              have different nexthops
    :rtype: list
    '''

    prefixes = []

    # dict of prefixes(str) : nexthops(str)
    _lhs = get_route_as_dict(lhs)
    _rhs = get_route_as_dict(rhs)
    common_prefixes = set(_lhs) & set(_rhs)

    for prefix in common_prefixes:
        if _lhs[prefix] != _rhs[prefix]:
            prefixes.append(prefix)

    return prefixes


def validate(routes_a, routes_b, sources, enable_color):

        extra_routes_in_a = routes_difference(routes_a, routes_b)
        extra_routes_in_b = routes_difference(routes_b, routes_a)
        diff_prefixes = prefixes_with_different_nexthops(routes_a, routes_b)

        # if all good, then return early
        if not extra_routes_in_a and not extra_routes_in_b and not diff_prefixes:
            if enable_color:
                click.echo(click.style('PASS', bg='green', fg='black'))
            else:
                click.echo('PASS')
            print('{} and {} routing table match'.format(*sources))
            return

        # Something failed.. report it
        if enable_color:
            click.echo(click.style('FAIL', bg='red', fg='black'))
        else:
            click.echo('FAIL')
        print('{} and {} routing table do not match'.format(*sources))
        if extra_routes_in_a:
            caption = 'Routes in {} but not in {}'.format(*sources)
            print_routes(caption, extra_routes_in_a)

        if extra_routes_in_b:
            caption = 'Routes in {} but not in {}'.format(*reversed(sources))
            print_routes(caption, extra_routes_in_b)

        if diff_prefixes:
            caption = 'Prefixes have different nexthops in {} and {}'.format(*sources)
            rows = []
            for prefix in diff_prefixes:
                rows.append([prefix])
            print(printing.render_vertical_table(rows, caption=caption))


def ip_nexthop_to_str(nh):
    '''
    Convert ttypes.BinaryAddress to string representation of a nexthop
    '''

    return "{}{}{}".format(utils.sprint_addr(nh.addr),
                           '@' if nh.ifName else '',
                           nh.ifName)


def print_routes(caption, routes):

    route_strs = []
    for route in routes:
        dest = utils.sprint_prefix(route.dest)
        paths_str = '\n'.join(["via {}".format(ip_nexthop_to_str(nh))
                               for nh in route.nexthops])
        route_strs.append((dest, paths_str))

    print(printing.render_vertical_table(route_strs, caption=caption))


class FibCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Fib client '''

        self.lm_cmd_port = cli_opts.lm_cmd_port

        self.client = fib_client.FibClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.fib_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)


class FibAgentCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Fib agent client '''

        self.lm_cmd_port = cli_opts.lm_cmd_port
        self.decision_rep_port = cli_opts.decision_rep_port
        try:
            self.client = utils.get_fib_agent_client(
                cli_opts.host,
                cli_opts.fib_agent_port,
                cli_opts.timeout,
                cli_opts.client_id
            )
        except Exception as e:
            print('Failed to get communicate to Fib. {}'.format(e))
            print('Note: Specify correct host with -H/--host option and ' +
                  'make sure that Fib is running on the host or ports ' +
                  'are open on that box for network communication.')
            sys.exit(1)


class FibLinuxAgentCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Linux Fib agent client '''

        self.lm_cmd_port = cli_opts.lm_cmd_port

        try:
            self.client = utils.get_fib_agent_client(
                cli_opts.host,
                cli_opts.fib_agent_port,
                cli_opts.timeout,
                cli_opts.client_id,
                LinuxFibService
            )
        except Exception as e:
            print('Failed to get communicate to Fib. {}'.format(e))
            print('Note: Specify correct host with -H/--host option and ' +
                  'make sure that Fib is running on the host or ports ' +
                  'are open on that box for network communication.')
            sys.exit(1)


class FibRoutesCmd(FibCmd):
    def run(self, json):
        route_db = self.client.get_route_db()
        if json:
            route_db_dict = {route_db.thisNodeName: utils.route_db_to_dict(route_db)}
            utils.print_routes_json(route_db_dict)
        else:
            utils.print_routes_table(route_db)


class FibCountersCmd(FibAgentCmd):
    def run(self):
        try:
            self.print_counters(self.client.getCounters())
        except Exception as e:
            print('Failed to get counter from Fib')
            print('Exception: {}'.format(e))
            sys.exit(1)

    def print_counters(self, counters):
        ''' print the Fib counters '''

        host_id = utils.get_connected_node_name(self.client.host, self.lm_cmd_port)
        caption = '{}\'s Fib counters'.format(host_id)

        rows = []
        for key in counters:
            rows.append(['{} : {}'.format(key, counters[key])])
        print(printing.render_horizontal_table(rows, caption=caption, tablefmt='plain'))
        print()


class FibListRoutesCmd(FibAgentCmd):
    def run(self):
        try:
            routes = self.client.getRouteTableByClient(self.client.client_id)
        except Exception as e:
            print('Failed to get routes from Fib.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        host_id = utils.get_connected_node_name(self.client.host, self.lm_cmd_port)
        caption = '{}\'s FIB routes by client {}'.format(host_id,
                                                         self.client.client_id)
        print_routes(caption, routes)


class FibAddRoutesCmd(FibAgentCmd):
    def run(self, prefixes, nexthops):
        routes = build_routes(prefixes.split(','), nexthops.split(','))

        try:
            self.client.addUnicastRoutes(self.client.client_id, routes)
        except Exception as e:
            print('Failed to add routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        print('Added {} routes.'.format(len(routes)))


class FibDelRoutesCmd(FibAgentCmd):
    def run(self, prefixes):
        prefixes = [utils.ip_str_to_prefix(p) for p in prefixes.split(',')]
        try:
            self.client.deleteUnicastRoutes(self.client.client_id, prefixes)
        except Exception as e:
            print('Failed to delete routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        print('Deleted {} routes.'.format(len(prefixes)))


class FibSyncRoutesCmd(FibAgentCmd):
    def run(self, prefixes, nexthops):
        routes = build_routes(prefixes.split(','), nexthops.split(','))

        try:
            self.client.syncFib(self.client.client_id, routes)
        except Exception as e:
            print('Failed to sync routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        print('Reprogrammed FIB with {} routes.'.format(len(routes)))


class FibValidateRoutesCmd(FibAgentCmd):
    def run(self, cli_opts):
        try:
            route_db = self.get_decision_route_db()
            fib_routes = self.client.getRouteTableByClient(self.client.client_id)
        except Exception as e:
            print('Failed to validate Fib routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        validate(self.get_routes(route_db), fib_routes, ['Decision', 'Fib'],
                 cli_opts.enable_color)

    def get_decision_route_db(self):
        self.decision_client = decision_client.DecisionClient(
            zmq.Context(),
            "tcp://[{}]:{}".format(self.client.host, self.decision_rep_port))
        return self.decision_client.get_route_db()

    def get_routes(self, route_db):
        '''
        Find all shortest routes for each prefix in routeDb
        '''

        shortest_routes = []
        for route in sorted(route_db.routes):
            if not route.paths:
                continue

            min_metric = min(route.paths, key=lambda x: x.metric).metric
            nexthops = []
            for path in route.paths:
                if path.metric == min_metric:
                    nexthops.append(path.nextHop)
                    nexthops[-1].ifName = path.ifName

            shortest_routes.append(ip_types.UnicastRoute(dest=route.prefix,
                                                         nexthops=nexthops))

        return shortest_routes


class FibListRoutesLinuxCmd(FibLinuxAgentCmd):
    def run(self):
        try:
            routes = self.client.getKernelRouteTable()
        except Exception as e:
            print('Failed to get routes from Fib.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        host_id = utils.get_connected_node_name(self.client.host, self.lm_cmd_port)
        caption = '{}\'s kernel routes'.format(host_id)
        print_routes(caption, routes)


class FibValidateRoutesLinuxCmd():
    def run(self, cli_opts):
        try:
            kernel_routes = FibLinuxAgentCmd(cli_opts).client.getKernelRouteTable()
            fib_routes = FibAgentCmd(cli_opts).client.getRouteTableByClient(
                cli_opts.client_id)
        except Exception as e:
            print('Failed to validate Fib routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        validate(kernel_routes, fib_routes, ['Kernel', 'Fib'], cli_opts.enable_color)
