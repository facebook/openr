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

import sys
import zmq

from openr.clients import fib_client, decision_client, lm_client
from openr.cli.utils import utils
from openr.utils import ipnetwork, printing


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


class FibRoutesComputedCmd(FibCmd):
    def run(self, prefixes, json):
        route_db = self.client.get_route_db()
        if json:
            route_db_dict = {route_db.thisNodeName: utils.route_db_to_dict(route_db)}
            utils.print_routes_json(route_db_dict, prefixes)
        else:
            utils.print_routes_table(route_db, prefixes)


class FibCountersCmd(FibAgentCmd):
    def run(self, json_opt):
        try:
            self.print_counters(self.client.getCounters(), json_opt)
            return 0
        except Exception as e:
            print('Failed to get counter from Fib')
            print('Exception: {}'.format(e))
            return 1

    def print_counters(self, counters, json_opt):
        ''' print the Fib counters '''

        host_id = utils.get_connected_node_name(self.client.host, self.lm_cmd_port)
        caption = '{}\'s Fib counters'.format(host_id)

        if json_opt:
            utils.print_json(counters)
        else:
            rows = []
            for key in counters:
                rows.append(['{} : {}'.format(key, counters[key])])
            print(printing.render_horizontal_table(
                rows, caption=caption, tablefmt='plain'))
            print()


class FibRoutesInstalledCmd(FibAgentCmd):
    def run(self, prefixes, json_opt):
        try:
            routes = self.client.getRouteTableByClient(self.client.client_id)
        except Exception as e:
            print('Failed to get routes from Fib.')
            print('Exception: {}'.format(e))
            return 1

        host_id = utils.get_connected_node_name(self.client.host, self.lm_cmd_port)
        client_id = self.client.client_id

        if json_opt:
            utils.print_json(utils.get_routes_json(
                host_id, client_id, routes, prefixes))
        else:
            caption = '{}\'s FIB routes by client {}'.format(host_id, client_id)
            utils.print_routes(caption, routes, prefixes)

        return 0


class FibAddRoutesCmd(FibAgentCmd):
    def run(self, prefixes, nexthops):
        routes = utils.build_routes(prefixes.split(','), nexthops.split(','))

        try:
            self.client.addUnicastRoutes(self.client.client_id, routes)
        except Exception as e:
            print('Failed to add routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        print('Added {} routes.'.format(len(routes)))


class FibDelRoutesCmd(FibAgentCmd):
    def run(self, prefixes):
        prefixes = [ipnetwork.ip_str_to_prefix(p) for p in prefixes.split(',')]
        try:
            self.client.deleteUnicastRoutes(self.client.client_id, prefixes)
        except Exception as e:
            print('Failed to delete routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        print('Deleted {} routes.'.format(len(prefixes)))


class FibSyncRoutesCmd(FibAgentCmd):
    def run(self, prefixes, nexthops):
        routes = utils.build_routes(prefixes.split(','), nexthops.split(','))

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
            decision_routes = self.get_decision_route_db(cli_opts)
            fib_routes = self.get_fib_route_db(cli_opts)
            agent_routes = self.client.getRouteTableByClient(
                self.client.client_id
            )
            lm_links = self.get_lm_link_db(cli_opts).interfaceDetails

        except Exception as e:
            print('Failed to validate Fib routes.')
            print('Exception: {}'.format(e))
            sys.exit(1)

        res1, _ = utils.compare_route_db(
            decision_routes,
            fib_routes,
            ['Decision', 'Openr-Fib'],
            cli_opts.enable_color,
        )
        res2, _ = utils.compare_route_db(
            fib_routes,
            agent_routes,
            ['Openr-Fib', 'FibAgent'],
            cli_opts.enable_color,
        )
        res3, _ = utils.validate_route_nexthops(
            fib_routes,
            lm_links,
            ['Openr-Fib', 'LinkMonitor'],
            cli_opts.enable_color,
        )
        return 0 if res1 and res2 and res3 else -1

    def get_fib_route_db(self, cli_opts):
        client = fib_client.FibClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.fib_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)
        return utils.get_shortest_routes(client.get_route_db())

    def get_decision_route_db(self, cli_opts):
        self.decision_client = decision_client.DecisionClient(
            zmq.Context(),
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.decision_rep_port))
        return utils.get_shortest_routes(self.decision_client.get_route_db())

    def get_lm_link_db(self, cli_opts):
        self.lm_client = lm_client.LMClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.lm_cmd_port),
            cli_opts.timeout,
            cli_opts.proto_factory)
        return self.lm_client.dump_links()
