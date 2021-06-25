#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import ipaddress
import sys
from collections import defaultdict
from typing import Any, Dict, List, Optional, Tuple

import click
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.Network import ttypes as network_types
from openr.OpenrCtrl import OpenrCtrl, ttypes as ctrl_types
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing
from openr.utils.consts import Consts
from openr.utils.serializer import deserialize_thrift_object


class DecisionPrefixesCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        nodes: set,
        json: bool,
        prefix: str,
        client_type: str,
        *args,
        **kwargs,
    ) -> None:
        prefix_dbs = client.getDecisionPrefixDbs()
        if json:
            utils.print_prefixes_json(
                prefix_dbs, nodes, prefix, client_type, self.iter_dbs
            )
        else:
            utils.print_prefixes_table(
                prefix_dbs, nodes, prefix, client_type, self.iter_dbs
            )


class DecisionRoutesComputedCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        nodes: set,
        prefixes: Any,
        labels: Any,
        json: bool,
        *args,
        **kwargs,
    ) -> None:
        if "all" in nodes:
            nodes = self._get_all_nodes(client)
        if json:
            route_db_dict = {}
            for node in nodes:
                route_db = client.getRouteDbComputed(node)
                route_db_dict[node] = utils.route_db_to_dict(route_db)
            utils.print_routes_json(route_db_dict, prefixes, labels)
        else:
            for node in nodes:
                route_db = client.getRouteDbComputed(node)
                utils.print_route_db(route_db, prefixes, labels)

    def _get_all_nodes(self, client: OpenrCtrl.Client) -> set:
        """return all the nodes' name in the network"""

        def _parse(nodes, adj_db):
            nodes.add(adj_db.thisNodeName)

        nodes = set()
        adj_dbs = client.getDecisionPrefixDbs()
        self.iter_dbs(nodes, adj_dbs, {"all"}, _parse)
        return nodes


class DecisionAdjCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        nodes: set,
        areas: set,
        bidir: bool,
        json: bool,
        *args,
        **kwargs,
    ) -> None:

        adj_dbs = client.getDecisionAdjacenciesFiltered(
            ctrl_types.AdjacenciesFilter(selectAreas=areas)
        )

        # convert list<adjDb> from server to a two level map: {area: {node: adjDb}}
        adjs_map_all_areas = utils.adj_dbs_to_area_dict(adj_dbs, nodes, bidir)

        if json:
            utils.print_json(adjs_map_all_areas)
        else:
            # print per-node adjDb tables on a per-area basis
            for area, adjs_map in sorted(adjs_map_all_areas.items()):
                print("\n== Area:", area, "==\n")
                utils.print_adjs_table(adjs_map, None, None)


class PathCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        src: str,
        dst: str,
        max_hop: int,
        area: str,
        *args,
        **kwargs,
    ) -> None:
        if not src or not dst:
            host_id = client.getMyNodeName()
            src = src or host_id
            dst = dst or host_id

        # pyre-fixme[16]: `PathCmd` has no attribute `prefix_dbs`.
        self.prefix_dbs: Dict[str, openr_types.PrefixDatabase] = {}
        area = utils.get_area_id(client, area)
        # Get prefix_dbs from KvStore

        params = openr_types.KeyDumpParams(Consts.PREFIX_DB_MARKER)
        params.keys = [Consts.PREFIX_DB_MARKER]
        if area is None:
            pub = client.getKvStoreKeyValsFiltered(params)
        else:
            pub = client.getKvStoreKeyValsFilteredArea(params, area)
        for value in pub.keyVals.values():
            utils.parse_prefix_database("", "", self.prefix_dbs, value)

        paths = self.get_paths(client, src, dst, max_hop)
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
            client = utils.get_fib_agent_client(src_addr, fib_agent_port, timeout)
            routes = client.getRouteTableByClient(client.client_id)
        except Exception:
            return []
        for route in routes:
            if ipnetwork.sprint_prefix(route.dest) == dst_prefix:
                return [nh.address for nh in route.nextHops]
        return []

    def get_paths(
        self, client: OpenrCtrl.Client, src: str, dst: str, max_hop: int
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

        adj_dbs = client.getDecisionAdjacencyDbs()
        if2node = self.get_if2node_map(adj_dbs)
        fib_routes = defaultdict(list)

        paths = []

        def _backtracking(cur, path, hop, visited, in_fib):
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
                client.getRouteDbComputed(cur),
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
                _backtracking(
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
        _backtracking(src, [], 1, visited_set, True)
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
    def _run(
        self, client: OpenrCtrl.Client, json=False, area: str = "", *args, **kwargs
    ) -> int:
        """Returns a status code. 0 = success, 1 = failure"""
        (decision_adj_dbs, decision_prefix_dbs, kvstore_keyvals) = self.get_dbs(
            client, area
        )

        kvstore_adj_node_names = set()
        kvstore_prefix_node_names = set()

        for key, value in sorted(kvstore_keyvals.items()):
            if key.startswith(Consts.ADJ_DB_MARKER):
                return_code = self.print_db_delta_adj(
                    key, value, kvstore_adj_node_names, decision_adj_dbs, json
                )
                if return_code != 0:
                    return return_code

        return_code = self.print_db_delta_prefix(
            kvstore_keyvals, kvstore_prefix_node_names, decision_prefix_dbs, json
        )
        if return_code != 0:
            return return_code

        decision_adj_node_names = {
            node
            for node in decision_adj_dbs.keys()
            if decision_adj_dbs[node].adjacencies
        }
        decision_prefix_node_names = set(decision_prefix_dbs.keys())

        adjValidateRet = self.print_db_diff(
            decision_adj_node_names,
            kvstore_adj_node_names,
            ["Decision", "KvStore"],
            "adj",
            json,
        )

        prefixValidateRet = self.print_db_diff(
            decision_prefix_node_names,
            kvstore_prefix_node_names,
            ["Decision", "KvStore"],
            "prefix",
            json,
        )

        return adjValidateRet or prefixValidateRet

    def get_dbs(self, client: OpenrCtrl.Client, area: str) -> Tuple[Dict, Dict, Dict]:
        # get LSDB from Decision
        decision_adj_dbs = client.getDecisionAdjacencyDbs()
        decision_prefix_dbs = client.getDecisionPrefixDbs()

        area = utils.get_area_id(client, area)
        # get LSDB from KvStore
        params = openr_types.KeyDumpParams(Consts.ALL_DB_MARKER)
        params.keys = [Consts.ALL_DB_MARKER]
        if area is None:
            kvstore_keyvals = client.getKvStoreKeyValsFiltered(params).keyVals
        else:
            kvstore_keyvals = client.getKvStoreKeyValsFilteredArea(params, area).keyVals

        return (decision_adj_dbs, decision_prefix_dbs, kvstore_keyvals)

    def print_db_delta_adj(
        self, key, value, kvstore_adj_node_names, decision_adj_dbs, json
    ):
        """Returns status code. 0 = success, 1 = failure"""

        kvstore_adj_db = deserialize_thrift_object(
            value.value, openr_types.AdjacencyDatabase
        )
        node_name = kvstore_adj_db.thisNodeName
        kvstore_adj_node_names.add(node_name)
        if node_name not in decision_adj_dbs:
            print(
                printing.render_vertical_table(
                    [["node {}'s adj db is missing in Decision".format(node_name)]]
                )
            )
            return 1
        decision_adj_db = decision_adj_dbs[node_name]

        return_code = 0
        if json:
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
                                "node {}'s adj db in Decision out of sync with "
                                "KvStore's".format(node_name)
                            ]
                        ]
                    )
                )
                print("\n".join(lines))
                return_code = 1

        return return_code

    def print_db_delta_prefix(
        self, kvstore_keyvals, kvstore_prefix_node_names, decision_prefix_dbs, json
    ):
        """Returns status code. 0 = success, 1 = failure"""

        prefix_maps = utils.collate_prefix_keys(kvstore_keyvals)

        for node_name, prefix_db in prefix_maps.items():
            kvstore_prefix_node_names.add(node_name)
            if node_name not in decision_prefix_dbs:
                print(
                    printing.render_vertical_table(
                        [
                            [
                                "node {}'s prefix db is missing in Decision".format(
                                    node_name
                                )
                            ]
                        ]
                    )
                )
                return 1
            decision_prefix_db = decision_prefix_dbs[node_name]
            decision_prefix_set = {}

            utils.update_global_prefix_db(decision_prefix_set, decision_prefix_db)
            lines = utils.sprint_prefixes_db_delta(decision_prefix_set, prefix_db)
            if lines:
                print(
                    printing.render_vertical_table(
                        [
                            [
                                "node {}'s prefix db in Decision out of sync with "
                                "KvStore's".format(node_name)
                            ]
                        ]
                    )
                )
                print("\n".join(lines))
                return 1
        return 0

    def print_db_diff(
        self,
        nodes_set_a: set,
        nodes_set_b: set,
        db_sources: List[str],
        db_type: str,
        json: bool,
    ) -> int:
        """Returns a status code, 0 = success, 1 = failure"""
        a_minus_b = sorted(nodes_set_a - nodes_set_b)
        b_minus_a = sorted(nodes_set_b - nodes_set_a)
        return_code = 0

        if json:
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
                print(printing.render_vertical_table(rows))
                return_code = 1

        if return_code == 1:
            if utils.is_color_output_supported():
                click.echo(click.style("FAIL", bg="red", fg="black"))
            else:
                click.echo("FAIL")
            print("{} table for {} and {} do not match".format(db_type, *db_sources))
        else:
            if utils.is_color_output_supported():
                click.echo(click.style("PASS", bg="green", fg="black"))
            else:
                click.echo("PASS")
            print("{} table for {} and {} match".format(db_type, *db_sources))

        return return_code


class DecisionRibPolicyCmd(OpenrCtrlCmd):
    # @override
    def _run(self, client: OpenrCtrl.Client, *args, **kwargs):
        policy = None
        try:
            policy = client.getRibPolicy()
        except ctrl_types.OpenrError as e:
            print("Error: ", str(e), "\nSystem standard error: ", sys.stderr)
            return

        # Convert the prefixes to readable format
        assert policy is not None

        # NOTE: We don't do explicit effor to print policy in
        print("> RibPolicy")
        print(f"  Validity: {policy.ttl_secs}s")
        for stmt in policy.statements:
            prefixes: List[str] = []
            if stmt.matcher.prefixes:
                prefixes = [
                    ipnetwork.sprint_prefix(p)
                    # pyre-fixme[16]: `Optional` has no attribute `__iter__`.
                    for p in stmt.matcher.prefixes
                ]
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
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        node: Optional[str],
        area: Optional[str],
        json: bool,
        detailed: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        routes = self.fetch(client, prefixes, node, area)

        # Print json if
        if json:
            # TODO: Print routes in json
            raise NotImplementedError()
        else:
            self.render(routes, detailed)

    def fetch(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        node: Optional[str],
        area: Optional[str],
    ) -> List[ctrl_types.ReceivedRouteDetail]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = ctrl_types.ReceivedRouteFilter()
        if prefixes:
            route_filter.prefixes = [ipnetwork.ip_str_to_prefix(p) for p in prefixes]
        if node:
            route_filter.nodeName = node
        if area:
            route_filter.areaName = area

        # Get routes
        return client.getReceivedRoutesFiltered(route_filter)

    def render(
        self,
        routes: List[ctrl_types.ReceivedRouteDetail],
        detailed: bool,
    ) -> None:
        """
        Render received routes
        """

        def key_fn(key: utils.PrintAdvertisedTypes) -> Tuple[str, str]:
            if not isinstance(key, ctrl_types.NodeAndArea):
                return ("", "")
            return (key.node, key.area)

        utils.print_route_details(routes, key_fn, detailed)
