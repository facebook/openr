#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import ipaddress
import sys
from collections import defaultdict
from collections.abc import Mapping, Sequence
from typing import Any, Dict, List, Optional, Set, Tuple

import click
from openr.py.openr.cli.utils import utils
from openr.py.openr.cli.utils.commands import OpenrCtrlCmd
from openr.py.openr.clients.openr_client import get_fib_agent_client
from openr.py.openr.utils import ipnetwork, printing
from openr.py.openr.utils.consts import Consts
from openr.py.openr.utils.serializer import serialize_json
from openr.thrift.KvStore import thrift_types as kv_store_types
from openr.thrift.Network import thrift_types as network_types
from openr.thrift.OpenrCtrl import thrift_types as ctrl_types
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Types import thrift_types as openr_types
from thrift.python.serializer import deserialize


class DecisionRoutesComputedCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        nodes: set,
        prefixes: Any,
        labels: Any,
        json_opt: bool,
        hostnames: bool = True,
        *args,
        **kwargs,
    ) -> None:
        if json_opt:
            route_db_dict = {}
            for node in nodes:
                route_db = await client.getRouteDbComputed(node)
                route_db_dict[node] = utils.route_db_to_dict(route_db)
            utils.print_routes_json(route_db_dict, prefixes, labels)
        else:
            nexthops_to_neighbor_names = None
            if hostnames:
                nexthops_to_neighbor_names = await utils.adjs_nexthop_to_neighbor_name(
                    client
                )
            for node in nodes:
                route_db = await client.getRouteDbComputed(node)
                utils.print_route_db(
                    route_db, prefixes, labels, nexthops_to_neighbor_names
                )


class DecisionShowPartialAdjCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        area: str,
        *args,
        **kwargs,
    ) -> None:
        adj_dbs = await client.getDecisionAdjacenciesFiltered(
            ctrl_types.AdjacenciesFilter(selectAreas={area})
        )
        # convert list<adjDb> from server to a two level map: {area: {node: adjDb}}
        adjs_map_all_areas = utils.adj_dbs_to_area_dict(
            adj_dbs, nodes={"all"}, bidir=False
        )

        print("\n== Area:", area, "==\n")
        adj_set = set()
        for _, adj_dbs in adjs_map_all_areas.items():
            for node_name, adj_db in adj_dbs.items():
                for adj in adj_db["adjacencies"]:
                    adj_set.add((node_name, adj["otherNodeName"]))
        print("Total adj (uni-directional):", len(adj_set))

        missing = []
        for node, adj in adj_set:
            r = (adj, node)
            if r not in adj_set:
                missing.append(r)
        print("Total partial adj:", len(missing))
        for missing_adj in sorted(missing):
            print(f"{missing_adj[0]} -X-> {missing_adj[1]}")


class DecisionAdjCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        nodes: set,
        areas: set,
        bidir: bool,
        json_opt: bool,
        *args,
        **kwargs,
    ) -> None:
        adj_dbs = await client.getDecisionAdjacenciesFiltered(
            ctrl_types.AdjacenciesFilter(selectAreas=areas)
        )

        # convert list<adjDb> from server to a two level map: {area: {node: adjDb}}
        adjs_map_all_areas = utils.adj_dbs_to_area_dict(adj_dbs, nodes, bidir)

        if json_opt:
            utils.print_json(adjs_map_all_areas)
        else:
            # print per-node adjDb tables on a per-area basis
            for area, adjs_map in sorted(adjs_map_all_areas.items()):
                print("\n== Area:", area, "==\n")
                utils.print_adjs_table(adjs_map, None, None)


class PathCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        src: str,
        dst: str,
        max_hop: int,
        area: str,
        *args,
        **kwargs,
    ) -> None:
        if not src or not dst:
            host_id = await client.getMyNodeName()
            src = src or host_id
            dst = dst or host_id

        # pyre-fixme[16]: `PathCmd` has no attribute `prefix_dbs`.
        self.prefix_dbs: dict[str, openr_types.PrefixDatabase] = {}
        area = await utils.get_area_id(client, area)
        # Get prefix_dbs from KvStore

        params = kv_store_types.KeyDumpParams(keys=[Consts.PREFIX_DB_MARKER])
        if area is None:
            pub = await client.getKvStoreKeyValsFiltered(params)
        else:
            pub = await client.getKvStoreKeyValsFilteredArea(params, area)
        for value in pub.keyVals.values():
            utils.parse_prefix_database("", "", self.prefix_dbs, value)

        paths = await self.get_paths(client, src, dst, max_hop)
        self.print_paths(paths)

    def get_loopback_addr(self, node):
        """get node's loopback addr"""

        def _parse(loopback_set, prefix_db):
            for prefix_entry in prefix_db.prefixEntries:
                # Only consider v6 address
                if len(prefix_entry.prefix.prefixAddress.addr) != 16:
                    continue

                # Parse PrefixAllocator address
                if prefix_entry.type == network_types.PrefixType.PREFIX_ALLOCATOR:
                    prefix = ipnetwork.sprint_prefix(prefix_entry.prefix)
                    if prefix_entry.prefix.prefixLength == 128:
                        prefix = prefix.split("/")[0]
                    else:
                        # TODO: we should ideally get address with last bit
                        # set to 1. `python3.6 ipaddress` libraries does this
                        # in one line. Alas no easy options with ipaddr
                        # NOTE: In our current usecase we are just assuming
                        # that allocated prefix has last 16 bits set to 0
                        prefix = prefix.split("/")[0] + "1"
                    loopback_set.add(prefix)
                    continue

                # Parse LOOPBACK address
                if prefix_entry.type == network_types.PrefixType.LOOPBACK:
                    prefix = ipnetwork.sprint_prefix(prefix_entry.prefix)
                    loopback_set.add(prefix.split("/")[0])
                    continue

        loopback_set = set()
        self.iter_dbs(loopback_set, self.prefix_dbs, node, _parse)
        return loopback_set.pop() if len(loopback_set) > 0 else None

    def get_node_prefixes(self, node, ipv4=False):
        def _parse(prefix_set, prefix_db):
            for prefix_entry in prefix_db.prefixEntries:
                if len(prefix_entry.prefix.prefixAddress.addr) == (4 if ipv4 else 16):
                    prefix_set.add(ipnetwork.sprint_prefix(prefix_entry.prefix))

        prefix_set = set()
        self.iter_dbs(prefix_set, self.prefix_dbs, node, _parse)
        return prefix_set

    def get_if2node_map(self, adj_dbs):
        """create a map from interface to node"""

        def _parse(if2node, adj_db):
            nexthop_dict = if2node[adj_db.thisNodeName]
            for adj in adj_db.adjacencies:
                nh6_addr = ipnetwork.sprint_addr(adj.nextHopV6.addr)
                nh4_addr = ipnetwork.sprint_addr(adj.nextHopV4.addr)
                nexthop_dict[(adj.ifName, nh6_addr)] = adj.otherNodeName
                nexthop_dict[(adj.ifName, nh4_addr)] = adj.otherNodeName

        if2node = defaultdict(dict)
        self.iter_dbs(if2node, adj_dbs, ["all"], _parse)
        return if2node

    def get_lpm_route(self, route_db, dst_addr):
        """find the routes to the longest prefix matches of dst."""

        max_prefix_len = -1
        lpm_route = None
        dst_addr = ipaddress.ip_address(dst_addr)
        for route in route_db.unicastRoutes:
            prefix = ipaddress.ip_network(ipnetwork.sprint_prefix(route.dest))
            if dst_addr in prefix:
                next_hop_prefix_len = route.dest.prefixLength
                if next_hop_prefix_len == max_prefix_len:
                    raise Exception(
                        "Duplicate prefix found in routing table {}".format(
                            ipnetwork.sprint_prefix(route.dest)
                        )
                    )
                elif next_hop_prefix_len > max_prefix_len:
                    lpm_route = route
                    max_prefix_len = next_hop_prefix_len

        return lpm_route

    def get_lpm_len_from_node(self, node, dst_addr):
        """
        return the longest prefix match of dst_addr in node's
        advertising prefix pool
        """

        cur_lpm_len = 0
        dst_addr = ipaddress.ip_address(dst_addr)
        is_ipv4 = isinstance(dst_addr, ipaddress.IPv4Address)
        for cur_prefix in self.get_node_prefixes(node, is_ipv4):
            if dst_addr in ipaddress.ip_network(cur_prefix):
                cur_len = int(cur_prefix.split("/")[1])
                cur_lpm_len = max(cur_lpm_len, cur_len)
        return cur_lpm_len

    def get_nexthop_nodes(
        self, route_db, dst_addr, cur_lpm_len, if2node, fib_routes, in_fib
    ):
        """get the next hop nodes.
        if the longest prefix is coming from the current node,
        return an empty list to terminate the path searching."""

        next_hop_nodes = []
        is_initialized = fib_routes[route_db.thisNodeName]

        lpm_route = self.get_lpm_route(route_db, dst_addr)
        is_ipv4 = isinstance(ipaddress.ip_address(dst_addr), ipaddress.IPv4Address)
        if lpm_route and lpm_route.dest.prefixLength >= cur_lpm_len:
            if in_fib and not is_initialized:
                fib_routes[route_db.thisNodeName].extend(
                    self.get_fib_path(
                        route_db.thisNodeName,
                        ipnetwork.sprint_prefix(lpm_route.dest),
                        self.fib_agent_port,
                        self.timeout,
                    )
                )
            min_cost = min(p.metric for p in lpm_route.nextHops)
            for nextHop in [p for p in lpm_route.nextHops if p.metric == min_cost]:
                if len(nextHop.address.addr) == (4 if is_ipv4 else 16):
                    nh_addr = ipnetwork.sprint_addr(nextHop.address.addr)
                    next_hop_node_name = if2node[route_db.thisNodeName][
                        (nextHop.address.ifName, nh_addr)
                    ]
                    next_hop_nodes.append(
                        [
                            next_hop_node_name,
                            nextHop.address.ifName,
                            nextHop.metric,
                            nh_addr,
                        ]
                    )
        return next_hop_nodes

    def get_fib_path(self, src, dst_prefix, fib_agent_port, timeout):
        src_addr = self.get_loopback_addr(src)
        if src_addr is None:
            return []

        try:
            client = get_fib_agent_client(src_addr, fib_agent_port, timeout)
            routes = client.getRouteTableByClient(client.client_id)
        except Exception:
            return []
        for route in routes:
            if ipnetwork.sprint_prefix(route.dest) == dst_prefix:
                return [nh.address for nh in route.nextHops]
        return []

    async def get_paths(
        self, client: OpenrCtrlCppClient.Async, src: str, dst: str, max_hop: int
    ) -> Any:
        """
        calc paths from src to dst using backtracking. can add memoization to
        convert to dynamic programming for better scalability when network is large.
        """

        dst_addr = dst
        # if dst is node, we get its loopback addr
        if ":" not in dst:
            dst_addr = self.get_loopback_addr(dst)
        try:
            ipaddress.ip_address(dst_addr)
        except ValueError:
            try:
                dst_addr = str(ipaddress.ip_network(dst, strict=False).network_address)
            except ValueError:
                print("node name or ip address not valid.")
                sys.exit(1)

        adj_dbs = await client.getDecisionAdjacencyDbs()
        if2node = self.get_if2node_map(adj_dbs)
        fib_routes = defaultdict(list)

        paths = []

        async def _backtracking(cur, path, hop, visited, in_fib):
            """
            Depth-first search (DFS) for traversing graph and getting paths
            from src to dst with lowest metric in total.

            Attributes:
                cur: current starting node
                path: a list of the nodes who form the path from src to the current node
                hop: how many hops from src node to the current node
                visited: a set of visited nodes
                in_fib: if current node is in fib path
            """
            if hop > max_hop:
                return

            # get the longest prefix match for dst_addr from current node's advertising prefixes
            cur_lpm_len = self.get_lpm_len_from_node(cur, dst_addr)
            # get the next hop nodes
            next_hop_nodes = self.get_nexthop_nodes(
                await client.getRouteDbComputed(cur),
                dst_addr,
                cur_lpm_len,
                if2node,
                fib_routes,
                in_fib,
            )

            if len(next_hop_nodes) == 0:
                if hop != 1:
                    paths.append((in_fib, path[:]))
                return

            for next_hop_node in next_hop_nodes:
                next_hop_node_name = next_hop_node[0]
                # prevent loops
                if next_hop_node_name in visited:
                    return

                path.append([hop] + next_hop_node)
                visited.add(next_hop_node_name)

                # check if next hop node is in fib path
                is_nexthop_in_fib_path = False
                for nexthop in fib_routes[cur]:
                    if (
                        next_hop_node[3] == ipnetwork.sprint_addr(nexthop.addr)
                        and next_hop_node[1] == nexthop.ifName
                    ):
                        is_nexthop_in_fib_path = True

                # recursion - extend the path from next hop node
                await _backtracking(
                    next_hop_node_name,
                    path,
                    hop + 1,
                    visited,
                    is_nexthop_in_fib_path and in_fib,
                )
                visited.remove(next_hop_node_name)
                path.pop()

        # initial call to begin the DFS to search paths
        visited_set = set()
        visited_set.add(src)
        await _backtracking(src, [], 1, visited_set, True)
        return paths

    def calculate_hop_metric(self, paths):
        """
        In the `paths` got from DFS, each hop only has the total metric toward the dst.

        For example, assuming there is a path from node0 to node3:
            node0 -(metric=50)-> node1 -(metric=20)-> node2 -(metric=80)-> node3

        In the `paths` argument, each hop only has the total metric toward the node3:
          Hop  NextHop Node    Interface      Metric (total metric toward the dst)
            1  node0           po1            150 (50+20+80)
            2  node1           po2            100 (20+80)
            3  node2           po3            80

        This function is trying to re-calculate the metric for each hop and make it like:
          Hop  NextHop Node    Interface      Metric (hop metric)
            1  node0           po1            50
            2  node1           po2            20
            3  node2           po3            80
        """
        if not paths:
            return

        for path in paths:
            path_hops = path[1]
            # only the path with more than one hop needed to re-caculculate
            if len(path_hops) <= 1:
                continue

            pre_total_metric = 0
            # iterate the hop metric in reverse way to do deduction, example:
            # Hop  NextHop Node    cur_total_metric   pre_total_metric  Hop Metric
            # 3    node2                 80                0            80-0    = 80
            # 2    node1                 100               80           100-80  = 20
            # 1    node0                 150               100          150-100 = 50
            for hop_attr in reversed(path_hops):
                cur_total_metric = hop_attr[3]
                hop_attr[3] = cur_total_metric - pre_total_metric
                pre_total_metric = cur_total_metric
        return paths

    def print_paths(self, paths):
        if not paths:
            print("No paths are found!")
            return

        paths = self.calculate_hop_metric(paths)

        column_labels = ["Hop", "NextHop Node", "Interface", "Metric", "NextHop-v6"]

        print(
            "{} {} found.".format(
                len(paths), "path is" if len(paths) == 1 else "paths are"
            )
        )

        for idx, path in enumerate(paths):
            print(
                printing.render_horizontal_table(
                    path[1],
                    column_labels,
                    caption="Path {}{}".format(idx + 1, "  *" if path[0] else ""),
                    tablefmt="plain",
                )
            )
            print()


class DecisionValidateCmd(OpenrCtrlCmd):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        json_opt: bool = False,
        suppress: bool = False,
        areas: Sequence[str] = (),
        *args: Any,
        **kwargs: Any,
    ) -> int:
        """Returns a status code. 0 = success, >= 1 failure"""

        # Global error counter
        errors = 0

        # Validate Open/R Initialization Event
        initialization_events = await client.getInitializationEvents()
        init_is_pass, init_err_msg_str = self.validate_init_event(
            initialization_events,
            kv_store_types.InitializationEvent.RIB_COMPUTED,
        )
        self.print_initialization_event_check(
            init_is_pass,
            init_err_msg_str,
            kv_store_types.InitializationEvent.RIB_COMPUTED,
            "decision",
        )

        if not init_is_pass:
            errors += 1

        # ATTN: validate cmd can run against specified area.
        # By default, it runs against ALL areas.
        if not areas:
            areas_summary = await client.getKvStoreAreaSummary(set())
            areas = tuple(a.area for a in areas_summary)

        for area in sorted(areas):
            click.secho(
                f"[Decision] Running validation checks on area: {area}", bold=True
            )
            (
                decision_adj_dbs,
                decision_prefix_dbs,
                kvstore_keyvals,
            ) = await self.get_dbs(client, area)

            # set to be populated by:
            #   - self.print_db_delta_adj();
            #   - self.print_db_delta_prefix();
            kvstore_adj_node_names = set()
            kvstore_prefix_node_names = set()

            # check adj db delta between decision and kvstore
            for key, value in sorted(kvstore_keyvals.items()):
                if key.startswith(Consts.ADJ_DB_MARKER):
                    return_code = self.print_db_delta_adj(
                        key, value, kvstore_adj_node_names, decision_adj_dbs, json_opt
                    )
                    if return_code:
                        errors += return_code
                        continue

            # check prefix db delta between decision and kvstore
            return_code, decision_prefix_node_names = self.print_db_delta_prefix(
                kvstore_keyvals,
                kvstore_prefix_node_names,
                decision_prefix_dbs,
                json_opt,
            )
            if return_code:
                errors += return_code
                continue

            decision_adj_node_names = {db.thisNodeName for db in decision_adj_dbs}

            errors += self.print_db_diff(
                decision_adj_node_names,
                kvstore_adj_node_names,
                ["Decision", "KvStore"],
                "adj",
                json_opt,
                suppress,
            )

            errors += self.print_db_diff(
                decision_prefix_node_names,
                kvstore_prefix_node_names,
                ["Decision", "KvStore"],
                "prefix",
                json_opt,
                suppress,
            )

        return errors

    async def get_dbs(
        self, client: OpenrCtrlCppClient.Async, area: str
    ) -> tuple[
        Sequence[openr_types.AdjacencyDatabase],
        Sequence[ctrl_types.ReceivedRouteDetail],
        Mapping[str, kv_store_types.Value],
    ]:
        # get LSDB from Decision
        adj_filter = ctrl_types.AdjacenciesFilter(selectAreas={area})
        decision_adj_dbs = await client.getDecisionAdjacenciesFiltered(adj_filter)
        route_filter = ctrl_types.ReceivedRouteFilter(areaName=area)
        decision_prefix_dbs = await client.getReceivedRoutesFiltered(route_filter)

        area_id = await utils.get_area_id(client, area)
        # get LSDB from KvStore
        params = kv_store_types.KeyDumpParams(keys=[Consts.ALL_DB_MARKER])
        if area_id is None:
            kvstore_publication = await client.getKvStoreKeyValsFiltered(params)
        else:
            kvstore_publication = await client.getKvStoreKeyValsFilteredArea(
                params, area_id
            )

        return (decision_adj_dbs, decision_prefix_dbs, kvstore_publication.keyVals)

    def print_db_delta_adj(
        self,
        key: str,
        value: Any,
        kvstore_adj_node_names: set,
        decision_adj_dbs: Sequence[openr_types.AdjacencyDatabase],
        json_opt: bool,
    ) -> int:
        """Returns status code. 0 = success, 1 = failure"""

        # fetch the adj database for one particular node
        if value.value:
            kvstore_adj_db = deserialize(openr_types.AdjacencyDatabase, value.value)
        else:
            kvstore_adj_db = openr_types.AdjacencyDatabase()

        node_name = kvstore_adj_db.thisNodeName
        kvstore_adj_node_names.add(node_name)
        if node_name not in {db.thisNodeName for db in decision_adj_dbs}:
            print(
                printing.render_vertical_table(
                    [[f"node {node_name}'s adj db is missing in Decision"]]
                )
            )
            return 1

        # fetch the corresponding node's adj database inside decision
        decision_adj_db = openr_types.AdjacencyDatabase()
        for db in decision_adj_dbs:
            if db.thisNodeName == node_name:
                decision_adj_db = db
                break

        # compare and find delta
        return_code = 0
        if json_opt:
            tags = ("in_decision", "in_kvstore", "changed_in_decision_and_kvstore")
            adj_list_deltas = utils.find_adj_list_deltas(
                decision_adj_db.adjacencies, kvstore_adj_db.adjacencies, tags=tags
            )
            deltas_json, return_code = utils.adj_list_deltas_json(
                adj_list_deltas, tags=tags
            )
            if return_code:
                utils.print_json(deltas_json)
        else:
            lines = utils.sprint_adj_db_delta(kvstore_adj_db, decision_adj_db)
            if lines:
                print(
                    printing.render_vertical_table(
                        [
                            [
                                f"node {node_name}'s adj db in Decision out of "
                                "sync with KvStore's"
                            ]
                        ]
                    )
                )
                print("\n".join(lines))
                return_code = 1

        return return_code

    def print_db_delta_prefix(
        self,
        kvstore_keyvals: Mapping[str, kv_store_types.Value],
        kvstore_prefix_node_names: set,
        decision_prefix_dbs: Sequence[ctrl_types.ReceivedRouteDetail],
        json_opt: bool,
    ) -> tuple[int, set[str]]:
        """Returns status code. 0 = success, 1 = failure"""

        prefix_maps = utils.collate_prefix_keys(kvstore_keyvals)
        decision_prefix_nodes = set()
        for received_route_detail in decision_prefix_dbs:
            for route in received_route_detail.routes:
                decision_prefix_nodes.add(route.key.node)

        for node_name, prefix_db in prefix_maps.items():
            kvstore_prefix_node_names.add(node_name)
            if node_name not in decision_prefix_nodes:
                print(
                    printing.render_vertical_table(
                        [[f"node {node_name}'s prefix db is missing in Decision"]]
                    )
                )
                return 1, decision_prefix_nodes

            decision_prefix_set = {}
            utils.update_global_prefix_db(decision_prefix_set, prefix_db)
            lines = utils.sprint_prefixes_db_delta(decision_prefix_set, prefix_db)
            if lines:
                print(
                    printing.render_vertical_table(
                        [
                            [
                                f"node {node_name}'s prefix db in Decision out of sync with KvStore's"
                            ]
                        ]
                    )
                )
                print("\n".join(lines))
                return 1, decision_prefix_nodes
        return 0, decision_prefix_nodes

    def print_db_diff(
        self,
        nodes_set_a: set,
        nodes_set_b: set,
        db_sources: list[str],
        db_type: str,
        json_opt: bool,
        suppress: bool,
    ) -> int:
        """Returns a status code, 0 = success, 1 = failure"""
        a_minus_b = sorted(nodes_set_a - nodes_set_b)
        b_minus_a = sorted(nodes_set_b - nodes_set_a)
        return_code = 0

        if json_opt:
            diffs_up = []
            diffs_down = []
            for node in a_minus_b:
                diffs_up.append(node)
            for node in b_minus_a:
                diffs_down.append(node)

            if diffs_up or diffs_down:
                diffs = {
                    "db_type": db_type,
                    "db_up": db_sources[0],
                    "db_down": db_sources[1],
                    "nodes_up": diffs_up,
                    "nodes_down": diffs_down,
                }
                return_code = 1
                utils.print_json(diffs)

        else:
            rows = []
            for node in a_minus_b:
                rows.append(
                    [
                        "node {}'s {} db in {} but not in {}".format(
                            node, db_type, *db_sources
                        )
                    ]
                )
            for node in b_minus_a:
                rows.append(
                    [
                        "node {}'s {} db in {} but not in {}".format(
                            node, db_type, *reversed(db_sources)
                        )
                    ]
                )
            if rows:
                if not suppress:
                    print(printing.render_vertical_table(rows))
                return_code = 1

        click.secho(
            self.validation_result_str(
                "decision",
                "{} table for {} and {} match check".format(db_type, *db_sources),
                return_code == 0,
            )
        )

        return return_code


class DecisionRibPolicyCmd(OpenrCtrlCmd):
    # @override
    async def _run(self, client: OpenrCtrlCppClient.Async, *args, **kwargs):
        policy = None
        try:
            policy = await client.getRibPolicy()
        except ctrl_types.OpenrError as e:
            print("Error: ", str(e), "\nSystem standard error: ", sys.stderr)
            return

        # Convert the prefixes to readable format
        assert policy is not None

        # NOTE: We don't do explicit effor to print policy in
        print("> RibPolicy")
        print(f"  Validity: {policy.ttl_secs}s")
        for stmt in policy.statements:
            prefixes: list[str] = []
            if stmt.matcher.prefixes:
                prefixes = [ipnetwork.sprint_prefix(p) for p in stmt.matcher.prefixes]
            tags = stmt.matcher.tags or []
            action = stmt.action.set_weight or ctrl_types.RibRouteActionWeight()
            print(f"  Statement: {stmt.name}")
            if prefixes:
                print(f"    Prefix Match List: {', '.join(prefixes)}")
            if tags:
                print(f"    Tags Match List: {', '.join(tags)}")
            print("    Action Set Weight:")
            print(f"      Default: {action.default_weight}")
            print("      Area:")
            for area, weight in action.area_to_weight.items():
                print(f"        {area}: {weight}")
            print("      Neighbor:")
            for neighbor, weight in action.neighbor_to_weight.items():
                print(f"        {neighbor}: {weight}")


class ReceivedRoutesCmd(OpenrCtrlCmd):
    # @override
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        prefixes: list[str],
        node: str | None,
        area: str | None,
        json_opt: bool,
        detailed: bool,
        tag2name: bool,
        *args,
        **kwargs,
    ) -> None:
        routes = await self.fetch(client, prefixes, node, area)

        if json_opt:
            print(serialize_json(routes))
        else:
            await self.render(routes, detailed, tag2name)

    async def fetch(
        self,
        client: OpenrCtrlCppClient.Async,
        prefixes: list[str],
        node: str | None,
        area: str | None,
    ) -> Sequence[ctrl_types.ReceivedRouteDetail]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = ctrl_types.ReceivedRouteFilter(
            prefixes=(
                [ipnetwork.ip_str_to_prefix(p) for p in prefixes] if prefixes else None
            ),
            nodeName=node,
            areaName=area,
        )

        # Get routes
        return await client.getReceivedRoutesFiltered(route_filter)

    async def render(
        self,
        routes: Sequence[ctrl_types.ReceivedRouteDetail],
        detailed: bool,
        tag2name: bool,
    ) -> None:
        """
        Render received routes
        """

        def key_fn(key: utils.PrintAdvertisedTypes) -> tuple[str, str]:
            if not isinstance(key, ctrl_types.NodeAndArea):
                return ("", "")
            return (key.node, key.area)

        tag_to_name = (
            utils.get_tag_to_name_map(await self._get_config()) if tag2name else None
        )

        utils.print_route_details(routes, key_fn, detailed, tag_to_name)
