#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from builtins import object
from typing import Any

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.clients import decision_client, fib_client, lm_client
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import ipnetwork, printing


class FibCmdBase(OpenrCtrlCmd):
    """
    Base class for Fib cmds. All of Fib cmd
    is spawn out of this.
    """

    def __init__(self, cli_opts):
        """initialize the Fib client"""
        super(FibCmdBase, self).__init__(cli_opts)


class FibAgentCmd(object):
    def __init__(self, cli_opts):
        """ initialize the Fib agent client """

        self.cli_opts = cli_opts
        self.decision_rep_port = cli_opts.decision_rep_port
        try:
            self.client = utils.get_fib_agent_client(
                cli_opts.host,
                cli_opts.fib_agent_port,
                cli_opts.timeout,
                cli_opts.client_id,
            )
        except Exception as e:
            print("Failed to get communicate to Fib. {}".format(e))
            print(
                "Note: Specify correct host with -H/--host option and "
                + "make sure that Fib is running on the host or ports "
                + "are open on that box for network communication."
            )
            raise


class FibRoutesComputedCmd(FibCmdBase):
    def _run(
        self, client: OpenrCtrl.Client, prefixes: Any, labels: Any, json: bool
    ) -> None:
        route_db = client.getRouteDb()
        if json:
            route_db_dict = {route_db.thisNodeName: utils.route_db_to_dict(route_db)}
            utils.print_routes_json(route_db_dict, prefixes, labels)
        else:
            utils.print_route_db(route_db, prefixes, labels)


class FibCountersCmd(FibAgentCmd):
    def run(self, json_opt):
        try:
            self.print_counters(self.client.getCounters(), json_opt)
            return 0
        except Exception as e:
            print("Failed to get counter from Fib")
            print("Exception: {}".format(e))
            return 1

    def print_counters(self, counters, json_opt):
        """ print the Fib counters """

        host_id = utils.get_connected_node_name(self.cli_opts)
        caption = "{}'s Fib counters".format(host_id)

        if json_opt:
            utils.print_json(counters)
        else:
            rows = []
            for key in counters:
                rows.append(["{} : {}".format(key, counters[key])])
            print(
                printing.render_horizontal_table(
                    rows, caption=caption, tablefmt="plain"
                )
            )
            print()


class FibRoutesInstalledCmd(FibAgentCmd):
    def run(self, prefixes, json_opt=False):
        try:
            routes = self.client.getRouteTableByClient(self.client.client_id)
        except Exception as e:
            print("Failed to get routes from Fib.")
            print("Exception: {}".format(e))
            return 1

        host_id = utils.get_connected_node_name(self.cli_opts)
        client_id = self.client.client_id

        if json_opt:
            utils.print_json(
                utils.get_routes_json(host_id, client_id, routes, prefixes)
            )
        else:
            caption = "{}'s FIB routes by client {}".format(host_id, client_id)
            utils.print_unicast_routes(caption, routes, prefixes)

        return 0


class FibAddRoutesCmd(FibAgentCmd):
    def run(self, prefixes, nexthops):
        routes = utils.build_routes(prefixes.split(","), nexthops.split(","))

        try:
            self.client.addUnicastRoutes(self.client.client_id, routes)
        except Exception as e:
            print("Failed to add routes.")
            print("Exception: {}".format(e))
            return 1

        print("Added {} routes.".format(len(routes)))
        return 0


class FibDelRoutesCmd(FibAgentCmd):
    def run(self, prefixes):
        prefixes = [ipnetwork.ip_str_to_prefix(p) for p in prefixes.split(",")]
        try:
            self.client.deleteUnicastRoutes(self.client.client_id, prefixes)
        except Exception as e:
            print("Failed to delete routes.")
            print("Exception: {}".format(e))
            return 1

        print("Deleted {} routes.".format(len(prefixes)))
        return 0


class FibSyncRoutesCmd(FibAgentCmd):
    def run(self, prefixes, nexthops):
        routes = utils.build_routes(prefixes.split(","), nexthops.split(","))

        try:
            self.client.syncFib(self.client.client_id, routes)
        except Exception as e:
            print("Failed to sync routes.")
            print("Exception: {}".format(e))
            return 1

        print("Reprogrammed FIB with {} routes.".format(len(routes)))
        return 0


class FibValidateRoutesCmd(FibAgentCmd):
    def run(self, cli_opts):
        try:
            # fetch routes from decision module
            (decision_unicast_routes, decision_mpls_routes) = utils.get_shortest_routes(
                decision_client.DecisionClient(cli_opts).get_route_db()
            )
            # fetch routes from fib module
            (fib_unicast_routes, fib_mpls_routes) = utils.get_shortest_routes(
                fib_client.FibClient(cli_opts).get_route_db()
            )
            # fetch route from net_agent module
            agent_unicast_routes = self.client.getRouteTableByClient(
                self.client.client_id
            )
            agent_mpls_routes = self.client.getMplsRouteTableByClient(
                self.client.client_id
            )
            # fetch link_db from link-monitor module
            lm_links = lm_client.LMClient(cli_opts).dump_links().interfaceDetails

        except Exception as e:
            print("Failed to validate Fib routes.")
            print("Exception: {}".format(e))
            raise e
            # return 1

        (res1_unicast, _) = utils.compare_route_db(
            decision_unicast_routes,
            fib_unicast_routes,
            "unicast",
            ["Decision:unicast", "Openr-Fib:unicast"],
            cli_opts.enable_color,
        )
        (res1_mpls, _) = utils.compare_route_db(
            decision_mpls_routes,
            fib_mpls_routes,
            "mpls",
            ["Decision:mpls", "Openr-Fib:mpls"],
            cli_opts.enable_color,
        )
        (res2_unicast, _) = utils.compare_route_db(
            fib_unicast_routes,
            agent_unicast_routes,
            "unicast",
            ["Openr-Fib:unicast", "FibAgent:unicast"],
            cli_opts.enable_color,
        )
        (res2_mpls, _) = utils.compare_route_db(
            fib_mpls_routes,
            agent_mpls_routes,
            "mpls",
            ["Openr-Fib:mpls", "FibAgent:mpls"],
            cli_opts.enable_color,
        )
        (res3, _) = utils.validate_route_nexthops(
            fib_unicast_routes,
            lm_links,
            ["Openr-Fib:unicast", "LinkMonitor"],
            cli_opts.enable_color,
        )
        return (
            0
            if res1_unicast and res1_mpls and res2_unicast and res2_mpls and res3
            else -1
        )
