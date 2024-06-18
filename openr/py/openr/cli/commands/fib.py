#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import asyncio
import datetime
import ipaddress
import time
from builtins import object
from typing import List, Optional, Sequence

import prettytable
import pytz
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.py.openr.clients.openr_client import get_fib_agent_client
from openr.thrift.Network.thrift_types import IpPrefix
from openr.thrift.OpenrCtrl.thrift_types import StreamSubscriberType
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Types.thrift_types import RouteDatabase, RouteDatabaseDelta
from openr.utils import ipnetwork, printing


class FibCmdBase(OpenrCtrlCmd):
    """define Fib specific methods here"""

    def __init__(self, cli_opts):
        super().__init__(cli_opts)

    def ip_key(self, ip: object):
        # pyre-fixme[6]: For 1st param expected `Union[bytes, int, IPv4Address,
        #  IPv4Interface, IPv4Network, IPv6Address, IPv6Interface, IPv6Network, str]` but
        #  got `object`.
        net = ipaddress.ip_network(ip)
        return (net.version, net.network_address, net.prefixlen)

    def convertTime(self, intTime) -> str:
        formatted_time = datetime.datetime.fromtimestamp(intTime / 1000)
        timezone = pytz.timezone("US/Pacific")
        formatted_time = timezone.localize(formatted_time)
        formatted_time = formatted_time.strftime("%Y-%m-%d %H:%M:%S.%f %Z")
        return formatted_time


class FibAgentCmd(FibCmdBase):
    def __init__(self, cli_opts):
        """initialize the Fib agent client"""

        super().__init__(cli_opts)
        try:
            self.fib_agent_client = get_fib_agent_client(
                host=cli_opts.host,
                port=cli_opts.fib_agent_port,
                timeout_ms=cli_opts.timeout,
                client_id=cli_opts.client_id,
            )
        except Exception as e:
            print("Failed to get communicate to Fib. {}".format(e))
            print(
                "Note: Specify correct host with -H/--host option and "
                + "make sure that Fib is running on the host or ports "
                + "are open on that box for network communication."
            )
            raise


class FibUnicastRoutesCmd(FibCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        prefix_or_ip: List[str],
        json: bool,
        hostnames: bool,
        *args,
        **kwargs,
    ) -> None:
        unicast_route_list = await client.getUnicastRoutesFiltered(prefix_or_ip)
        nexthops_to_neighbor_names = {}
        if hostnames:
            nexthops_to_neighbor_names = await utils.adjs_nexthop_to_neighbor_name(
                client
            )
        host_name = await client.getMyNodeName()

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
                f"Unicast Routes for {host_name}",
                unicast_route_list,
                nexthops_to_neighbor_names=nexthops_to_neighbor_names,
            )


class FibMplsRoutesCmd(FibCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        labels: List[int],
        json: bool,
        *args,
        **kwargs,
    ) -> None:
        int_label_filters = [int(label) for label in labels]
        mpls_route_list = await client.getMplsRoutesFiltered(int_label_filters)
        host_name = await client.getMyNodeName()

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
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self, client: OpenrCtrlCppClient.Async, json_opt, *args, **kwargs
    ) -> int:
        try:
            self.print_counters(
                self.fib_agent_client.getCounters(),
                await client.getMyNodeName(),
                json_opt,
            )
            return 0
        except Exception as e:
            print("Failed to get counter from Fib")
            print("Exception: {}".format(e))
            return 1

    def print_counters(self, counters, host_id, json_opt) -> None:
        """print the Fib counters"""

        caption = f"{host_id}'s Fib counters"

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
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        prefixes: List[str],
        labels: Optional[List[int]] = None,
        json_opt: bool = False,
        client_id: Optional[int] = None,
        *args,
        **kwargs,
    ):
        routes = []
        mpls_routes = []
        client_id = (
            client_id if client_id is not None else self.fib_agent_client.client_id
        )

        try:
            routes = self.fib_agent_client.getRouteTableByClient(clientId=client_id)
        except Exception as e:
            print("Failed to get routes from Fib.")
            print("Exception: {}".format(e))
            return 1

        try:
            mpls_routes = self.fib_agent_client.getMplsRouteTableByClient(
                clientId=client_id
            )
        except Exception:
            pass

        if json_opt:
            utils.print_json(
                utils.get_routes_json(
                    "", client_id, routes, prefixes, mpls_routes, labels
                )
            )
        else:
            caption = f"FIB routes by client {client_id}"
            utils.print_unicast_routes(caption, routes, prefixes)
            caption = f"MPLS routes by client {client_id}"
            utils.print_mpls_routes(caption, mpls_routes, labels)

        return 0


class FibAddRoutesCmd(FibAgentCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self, client: OpenrCtrlCppClient.Async, prefixes, nexthops, *args, **kwargs
    ):
        routes = utils.build_routes(prefixes.split(","), nexthops.split(","))

        try:
            self.fib_agent_client.addUnicastRoutes(
                self.fib_agent_client.client_id, routes
            )
        except Exception as e:
            print("Failed to add routes.")
            print("Exception: {}".format(e))
            return 1

        print("Added {} routes.".format(len(routes)))
        return 0


class FibDelRoutesCmd(FibAgentCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(self, client: OpenrCtrlCppClient.Async, prefixes, *args, **kwargs):
        prefixes = [ipnetwork.ip_str_to_prefix(p) for p in prefixes.split(",")]
        try:
            self.fib_agent_client.deleteUnicastRoutes(
                self.fib_agent_client.client_id, prefixes
            )
        except Exception as e:
            print("Failed to delete routes.")
            print("Exception: {}".format(e))
            return 1

        print("Deleted {} routes.".format(len(prefixes)))
        return 0


class FibSyncRoutesCmd(FibAgentCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self, client: OpenrCtrlCppClient.Async, prefixes, nexthops, *args, **kwargs
    ):
        routes = utils.build_routes(prefixes.split(","), nexthops.split(","))

        try:
            self.fib_agent_client.syncFib(self.fib_agent_client.client_id, routes)
        except Exception as e:
            print("Failed to sync routes.")
            print("Exception: {}".format(e))
            return 1

        print("Reprogrammed FIB with {} routes.".format(len(routes)))
        return 0


class FibValidateRoutesCmd(FibAgentCmd):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        suppress_error=False,
        *args,
        **kwargs,
    ) -> int:

        all_success = True

        # fetch openr config for comparison
        openr_config = await client.getRunningConfigThrift()

        try:
            decision_route_db = None
            fib_route_db = None
            lm_links = None

            # fetch routes from decision module
            decision_route_db = await client.getRouteDbComputed("")
            # fetch routes from fib module
            fib_route_db = await client.getRouteDb()
            # fetch link_db from link-monitor module
            lm_links = (await client.getInterfaces()).interfaceDetails

            (decision_unicast_routes, decision_mpls_routes) = utils.get_routes(
                decision_route_db
            )
            (fib_unicast_routes, fib_mpls_routes) = utils.get_routes(fib_route_db)

        except Exception as e:
            print("Failed to validate Fib routes.")
            print("Exception: {}".format(e))
            raise e

        # compare UNICAST route database between Decision and Fib module
        (ret, _) = utils.compare_route_db(
            decision_unicast_routes,
            fib_unicast_routes,
            "unicast",
            ["Openr-Decision:unicast", "Openr-Fib:unicast"],
            suppress_error,
        )
        all_success = all_success and ret

        # compare MPLS route database between Decision and Fib module
        (ret, _) = utils.compare_route_db(
            decision_mpls_routes,
            fib_mpls_routes,
            "mpls",
            ["Openr-Decision:mpls", "Openr-Fib:mpls"],
            suppress_error,
        )
        all_success = all_success and ret

        # ATTN: with dryrun=true. Fib module will skip programming routes
        if not openr_config.dryrun:
            agent_unicast_routes = self.fib_agent_client.getRouteTableByClient(
                clientId=self.fib_agent_client.client_id
            )

            # compare UNICAST route database between Fib module and FibAgent
            (ret, _) = utils.compare_route_db(
                fib_unicast_routes,
                agent_unicast_routes,
                "unicast",
                ["Openr-Fib:unicast", "FibAgent:unicast"],
                suppress_error,
            )
            all_success = all_success and ret

            # compare MPLS route database between Fib module and FibAgent
            try:
                agent_mpls_routes = self.fib_agent_client.getMplsRouteTableByClient(
                    clientId=self.fib_agent_client.client_id
                )
                (ret, _) = utils.compare_route_db(
                    fib_mpls_routes,
                    agent_mpls_routes,
                    "mpls",
                    ["Openr-Fib:mpls", "FibAgent:mpls"],
                    suppress_error,
                )
                all_success = all_success and ret
            except Exception:
                pass

        # validate UNICAST routes nexthops
        (ret, _) = utils.validate_route_nexthops(
            fib_unicast_routes,
            lm_links,
            ["Openr-Fib:unicast", "LinkMonitor"],
        )
        all_success = all_success and ret

        return 0 if all_success else -1


class FibSnoopCmd(FibCmdBase):
    def print_ip_prefixes_filtered(
        self,
        ip_prefixes: Sequence[IpPrefix],
        prefixes_filter: Optional[List[str]] = None,
        element_prefix: str = ">",
        element_suffix: str = "",
    ) -> None:
        """
        Print prefixes. If specified, exact match subset of prefixes_filter
        only will be printed.
        :param unicast_routes: Unicast routes
        :param prefixes_filter: Optional prefixes/filter to print (Exact match).
        :param element_prefix: Starting prefix for each item. (string)
        :param element_suffix: Ending/terminator for each item. (string)
        """

        filter_for_networks = None
        if prefixes_filter:
            filter_for_networks = [ipaddress.ip_network(p) for p in prefixes_filter]

        prefix_strs = []
        for ip_prefix in ip_prefixes:
            if (
                filter_for_networks
                and not ipaddress.ip_network(ipnetwork.sprint_prefix(ip_prefix))
                in filter_for_networks
            ):
                continue

            prefix_strs.append([ipnetwork.sprint_prefix(ip_prefix)])

        print(
            printing.render_vertical_table(
                prefix_strs,
                element_prefix=element_prefix,
                element_suffix=element_suffix,
                timestamp=True,
            )
        )

    def print_mpls_labels(
        self,
        labels: Sequence[int],
        element_prefix: str = ">",
        element_suffix: str = "",
    ) -> None:
        """
        Print mpls labels. Subset specified by labels_filter only will be printed if specified
        :param labels: mpls labels
        :param element_prefix: Starting prefix for each item. (string)
        :param element_suffix: Ending/terminator for each item. (string)
        """

        label_strs = [[str(label)] for label in labels]

        print(
            printing.render_vertical_table(
                label_strs,
                element_prefix=element_prefix,
                element_suffix=element_suffix,
                timestamp=True,
            )
        )

    def print_route_db_delta(
        self,
        delta_db: RouteDatabaseDelta,
        prefixes: Optional[List[str]] = None,
    ) -> None:
        """print the RouteDatabaseDelta from Fib module"""

        if len(delta_db.unicastRoutesToUpdate) != 0:
            utils.print_unicast_routes(
                caption="",
                unicast_routes=delta_db.unicastRoutesToUpdate,
                prefixes=prefixes,
                element_prefix="+",
                filter_exact_match=True,
                timestamp=True,
            )
        if len(delta_db.unicastRoutesToDelete) != 0:
            self.print_ip_prefixes_filtered(
                ip_prefixes=delta_db.unicastRoutesToDelete,
                prefixes_filter=prefixes,
                element_prefix="-",
            )

        if prefixes:
            return

        if len(delta_db.mplsRoutesToUpdate) != 0:
            utils.print_mpls_routes(
                caption="",
                mpls_routes=delta_db.mplsRoutesToUpdate,
                element_prefix="+",
                element_suffix="(MPLS)",
                timestamp=True,
            )
        if len(delta_db.mplsRoutesToDelete) != 0:
            self.print_mpls_labels(
                labels=delta_db.mplsRoutesToDelete,
                element_prefix="-",
                element_suffix="(MPLS)",
            )

    def print_route_db(
        self,
        route_db: RouteDatabase,
        prefixes: Optional[List[str]] = None,
        labels: Optional[List[int]] = None,
    ) -> None:
        """print the routes from Fib module"""

        if (prefixes or not labels) and len(route_db.unicastRoutes) != 0:
            utils.print_unicast_routes(
                caption="",
                unicast_routes=route_db.unicastRoutes,
                prefixes=prefixes,
                element_prefix="+",
                filter_exact_match=True,
                timestamp=True,
            )
        if (labels or not prefixes) and len(route_db.mplsRoutes) != 0:
            utils.print_mpls_routes(
                caption="",
                mpls_routes=route_db.mplsRoutes,
                labels=labels,
                element_prefix="+",
                element_suffix="(MPLS)",
                timestamp=True,
            )

    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        duration: int,
        initial_dump: bool,
        prefixes: List[str],
        *args,
        **kwargs,
    ) -> None:

        initialDb, updates = await client.subscribeAndGetFib()
        # Print summary
        print(f" Routes for {initialDb.thisNodeName}.")
        print(f" {len(initialDb.unicastRoutes)} unicast routes in initial dump.")
        print(f" {len(initialDb.mplsRoutes)} mpls routes in initial dump.\n")
        # Expand initial dump based on input argument
        if initial_dump:
            self.print_route_db(initialDb, prefixes)

        print("RouteDatabaseDelta updates to follow ...\n")

        start_time = time.time()
        awaited_updates = None
        while True:
            # Break if it is time
            if duration > 0 and time.time() - start_time > duration:
                print("Duration expired. Terminating snooping.")
                break

            # Await for an update
            if not awaited_updates:
                awaited_updates = [updates.__anext__()]
            done, awaited_updates = await asyncio.wait(awaited_updates, timeout=1)
            if not done:
                continue
            else:
                msg = await done.pop()

            self.print_route_db_delta(msg, prefixes)


class StreamSummaryCmd(FibCmdBase):
    def get_subscriber_row(self, stream_session_info):
        """
        Takes StreamSubscriberInfo from thrift and returns list[str] (aka row)
        representing the subscriber
        """

        uptime = "unknown"
        last_msg_time = "unknown"
        if (
            stream_session_info.uptime is not None
            and stream_session_info.last_msg_sent_time is not None
        ):
            uptime_str = str(
                datetime.timedelta(milliseconds=stream_session_info.uptime)
            )
            last_msg_time_str = self.convertTime(stream_session_info.last_msg_sent_time)
            uptime = uptime_str.split(".")[0]
            last_msg_time = last_msg_time_str

        # assert isinstance(stream_session_info, StreamSubscriberInfo)
        row = [
            stream_session_info.subscriber_id,
            uptime,
            stream_session_info.total_streamed_msgs,
            last_msg_time,
        ]
        return row

    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        *args,
        **kwargs,
    ) -> None:

        subscribers = await client.getSubscriberInfo(StreamSubscriberType.FIB)

        # Prepare the table
        columns = [
            "SubscriberID",
            "Uptime",
            "Total Messages",
            "Time of Last Message",
        ]
        table = ""
        table = prettytable.PrettyTable(columns)
        table.set_style(prettytable.PLAIN_COLUMNS)
        table.align = "l"
        table.left_padding_width = 0
        table.right_padding_width = 2

        for subscriber in sorted(
            subscribers, key=lambda x: self.ip_key(x.subscriber_id)
        ):
            table.add_row(self.get_subscriber_row(subscriber))

        # Print header
        print("FIB stream summary information for subscribers\n")

        if not subscribers:
            print("No subscribers available")

        # Print the table body
        print(table)
