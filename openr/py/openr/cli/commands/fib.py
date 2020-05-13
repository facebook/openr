#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from builtins import object
from typing import Any, List, Optional

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.clients.openr_client import get_openr_ctrl_client
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import ipnetwork, printing


class FibAgentCmd(object):
    def __init__(self, cli_opts):
        """ initialize the Fib agent client """
        self.cli_opts = cli_opts
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


class FibUnicastRoutesCmd(OpenrCtrlCmd):
    def _run(
        self, client: OpenrCtrl.Client, prefix_or_ip: List[str], json: bool
    ) -> None:
        unicast_route_list = client.getUnicastRoutesFiltered(prefix_or_ip)
        host_name = client.getMyNodeName()

        if json:
            routes = {
                "unicastRoutes": [
                    utils.unicast_route_to_dict(r) for r in unicast_route_list
                ]
            }
            route_dict = {host_name: routes}
            utils.print_routes_json(route_dict)
        else:
            utils.print_unicast_routes(
                "Unicast Routes for {}".format(host_name), unicast_route_list
            )


class FibMplsRoutesCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client, labels: List[int], json: bool) -> None:
        int_label_filters = [int(label) for label in labels]
        mpls_route_list = client.getMplsRoutesFiltered(int_label_filters)
        host_name = client.getMyNodeName()

        if json:
            routes = {
                "mplsRoutes": [utils.mpls_route_to_dict(r) for r in mpls_route_list]
            }
            route_dict = {host_name: routes}
            utils.print_routes_json(route_dict)
        else:
            utils.print_mpls_routes(
                "MPLS Routes for {}".format(host_name), mpls_route_list
            )


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

        with utils.get_openr_ctrl_client(self.cli_opts.host, self.cli_opts) as client:
            host_id = client.getMyNodeName()
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
    def run(
        self,
        prefixes: List[str],
        labels: List[int] = (),
        json_opt: bool = False,
        client_id: Optional[int] = None,
    ):
        routes = []
        mpls_routes = []
        client_id = client_id if client_id is not None else self.client.client_id

        try:
            routes = self.client.getRouteTableByClient(client_id)
        except Exception as e:
            print("Failed to get routes from Fib.")
            print("Exception: {}".format(e))
            return 1

        with utils.get_openr_ctrl_client(self.cli_opts.host, self.cli_opts) as client:
            host_id = client.getMyNodeName()

        try:
            mpls_routes = self.client.getMplsRouteTableByClient(client_id)
        except Exception:
            pass

        if json_opt:
            utils.print_json(
                utils.get_routes_json(
                    host_id, client_id, routes, prefixes, mpls_routes, labels
                )
            )
        else:
            caption = f"{host_id}'s FIB routes by client {client_id}"
            utils.print_unicast_routes(caption, routes, prefixes)
            caption = f"{host_id}'s MPLS routes by client {client_id}"
            utils.print_mpls_routes(caption, mpls_routes, labels)

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
        all_success = True

        try:
            decision_route_db = None
            fib_route_db = None
            lm_links = None

            with get_openr_ctrl_client(cli_opts.host, cli_opts) as client:
                # fetch routes from decision module
                decision_route_db = client.getRouteDbComputed("")
                # fetch routes from fib module
                fib_route_db = client.getRouteDb()
                # fetch link_db from link-monitor module
                lm_links = client.getInterfaces().interfaceDetails

            (decision_unicast_routes, decision_mpls_routes) = utils.get_routes(
                decision_route_db
            )
            (fib_unicast_routes, fib_mpls_routes) = utils.get_routes(fib_route_db)
            # fetch route from net_agent module
            agent_unicast_routes = self.client.getRouteTableByClient(
                self.client.client_id
            )

        except Exception as e:
            print("Failed to validate Fib routes.")
            print("Exception: {}".format(e))
            raise e
            # return 1

        (ret, _) = utils.compare_route_db(
            decision_unicast_routes,
            fib_unicast_routes,
            "unicast",
            ["Decision:unicast", "Openr-Fib:unicast"],
        )
        all_success = all_success and ret

        (ret, _) = utils.compare_route_db(
            decision_mpls_routes,
            fib_mpls_routes,
            "mpls",
            ["Decision:mpls", "Openr-Fib:mpls"],
        )
        all_success = all_success and ret

        (ret, _) = utils.compare_route_db(
            fib_unicast_routes,
            agent_unicast_routes,
            "unicast",
            ["Openr-Fib:unicast", "FibAgent:unicast"],
        )
        all_success = all_success and ret

        # for backward compatibily of Open/R binary
        try:
            agent_mpls_routes = self.client.getMplsRouteTableByClient(
                self.client.client_id
            )
            (ret, _) = utils.compare_route_db(
                fib_mpls_routes,
                agent_mpls_routes,
                "mpls",
                ["Openr-Fib:mpls", "FibAgent:mpls"],
            )
            all_success = all_success and ret
        except Exception:
            pass

        (ret, _) = utils.validate_route_nexthops(
            fib_unicast_routes, lm_links, ["Openr-Fib:unicast", "LinkMonitor"]
        )
        all_success = all_success and ret

        return 0 if all_success else -1
