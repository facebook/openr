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

from openr.clients import decision_client, kvstore_client
from openr.cli.utils import utils
from openr.utils import ipnetwork, printing
from openr.utils.serializer import deserialize_thrift_object
from openr.Lsdb import ttypes as lsdb_types
from openr.utils.consts import Consts

from collections import defaultdict
import ipaddress
import sys


class DecisionCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Decision client '''

        self.host = cli_opts.host
        self.timeout = cli_opts.timeout
        self.lm_cmd_port = cli_opts.lm_cmd_port
        self.kv_rep_port = cli_opts.kv_rep_port
        self.fib_agent_port = cli_opts.fib_agent_port
        self.enable_color = cli_opts.enable_color

        self.client = decision_client.DecisionClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.decision_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)
        self.kvstore_client = kvstore_client.KvStoreClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.kv_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)

    def iter_dbs(self, container, dbs, nodes, parse_func):
        ''' parse prefix databases from decision module

            :container: container to store the generated data
            :dbs decision_types.PrefixDbs or decision_types.AdjDbs
            :nodes set: the set of nodes for parsing
            :parse_func function: the parsing function
        '''

        for (node, db) in sorted(dbs.items()):
            if 'all' not in nodes and node not in nodes:
                continue
            parse_func(container, db)


class DecisionPrefixesCmd(DecisionCmd):
    def run(self, nodes, json):
        prefix_dbs = self.client.get_prefix_dbs()
        if json:
            utils.print_prefixes_json(prefix_dbs, nodes, self.iter_dbs)
        else:
            utils.print_prefixes_table(prefix_dbs, nodes, self.iter_dbs)


class DecisionRoutesComputedCmd(DecisionCmd):
    def run(self, nodes, prefixes, json):
        if 'all' in nodes:
            nodes = self._get_all_nodes()
        if json:
            route_db_dict = {}
            for node in nodes:
                route_db = self.client.get_route_db(node)
                route_db_dict[node] = utils.route_db_to_dict(route_db)
            utils.print_routes_json(route_db_dict, prefixes)
        else:
            for node in nodes:
                route_db = self.client.get_route_db(node)
                utils.print_routes_table(route_db, prefixes)

    def _get_all_nodes(self):
        ''' return all the nodes' name in the network '''

        def _parse(nodes, adj_db):
            nodes.add(adj_db.thisNodeName)

        nodes = set()
        adj_dbs = self.client.get_prefix_dbs()
        self.iter_dbs(nodes, adj_dbs, ['all'], _parse)
        return nodes


class DecisionAdjCmd(DecisionCmd):
    def run(self, nodes, bidir, json):
        adj_dbs = self.client.get_adj_dbs()
        adjs_map = utils.adj_dbs_to_dict(adj_dbs, nodes, bidir, self.iter_dbs)
        if json:
            utils.print_json(adjs_map)
        else:
            utils.print_adjs_table(adjs_map, self.enable_color, None, None)


class PathCmd(DecisionCmd):
    def run(self, src, dst, max_hop):
        if not src or not dst:
            host_id = utils.get_connected_node_name(self.host, self.lm_cmd_port)
            src = src or host_id
            dst = dst or host_id

        # Get prefix_dbs from KvStore
        self.prefix_dbs = {}
        pub = self.kvstore_client.dump_all_with_prefix(Consts.PREFIX_DB_MARKER)
        for v in pub.keyVals.values():
            prefix_db = deserialize_thrift_object(
                v.value, lsdb_types.PrefixDatabase)
            self.prefix_dbs[prefix_db.thisNodeName] = prefix_db

        paths = self.get_paths(src, dst, max_hop)
        self.print_paths(paths)

    def get_loopback_addr(self, node):
        ''' get node's loopback addr'''

        def _parse(loopback_set, prefix_db):
            for prefix_entry in prefix_db.prefixEntries:
                # Only consider v6 address
                if len(prefix_entry.prefix.prefixAddress.addr) != 16:
                    continue

                # Parse PrefixAllocator address
                if prefix_entry.type == lsdb_types.PrefixType.PREFIX_ALLOCATOR:
                    prefix = ipnetwork.sprint_prefix(prefix_entry.prefix)
                    if prefix_entry.prefix.prefixLength == 128:
                        prefix = prefix.split('/')[0]
                    else:
                        # TODO: we should ideally get address with last bit
                        # set to 1. `python3.6 ipaddress` libraries does this
                        # in one line. Alas no easy options with ipaddr
                        # NOTE: In our current usecase we are just assuming
                        # that allocated prefix has last 16 bits set to 0
                        prefix = prefix.split('/')[0] + '1'
                    loopback_set.add(prefix)
                    continue

                # Parse LOOPBACK address
                if prefix_entry.type == lsdb_types.PrefixType.LOOPBACK:
                    prefix = ipnetwork.sprint_prefix(prefix_entry.prefix)
                    loopback_set.add(prefix.split('/')[0])
                    continue

        loopback_set = set()
        self.iter_dbs(loopback_set, self.prefix_dbs, node, _parse)
        return loopback_set.pop() if len(loopback_set) > 0 else None

    def get_node_prefixes(self, node):

        def _parse(prefix_set, prefix_db):
            for prefix_entry in prefix_db.prefixEntries:
                if len(prefix_entry.prefix.prefixAddress.addr) == 16:
                    prefix_set.add(ipnetwork.sprint_prefix(prefix_entry.prefix))

        prefix_set = set()
        self.iter_dbs(prefix_set, self.prefix_dbs, node, _parse)
        return prefix_set

    def get_if2node_map(self, adj_dbs):
        ''' create a map from interface to node '''

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
        ''' find the routes to the longest prefix matches of dst. '''

        max_prefix_len = -1
        lpm_route = None
        dst_addr = ipaddress.ip_address(dst_addr)
        for route in route_db.routes:
            prefix = ipaddress.ip_network(ipnetwork.sprint_prefix(route.prefix))
            if dst_addr in prefix:
                next_hop_prefix_len = route.prefix.prefixLength
                if next_hop_prefix_len == max_prefix_len:
                    raise Exception('Duplicate prefix found in routing table {}'
                                    .format(ipnetwork.sprint_prefix(route.prefix)))
                elif next_hop_prefix_len > max_prefix_len:
                    lpm_route = route
                    max_prefix_len = next_hop_prefix_len

        return lpm_route

    def get_lpm_len_from_node(self, node, dst_addr):
        '''
        return the longest prefix match of dst_addr in node's
        advertising prefix pool
        '''

        cur_lpm_len = 0
        dst_addr = ipaddress.ip_address(dst_addr)
        for cur_prefix in self.get_node_prefixes(node):
            if dst_addr in ipaddress.ip_network(cur_prefix):
                cur_len = int(cur_prefix.split('/')[1])
                cur_lpm_len = max(cur_lpm_len, cur_len)
        return cur_lpm_len

    def get_nexthop_nodes(self, route_db, dst_addr, cur_lpm_len,
                          if2node, fib_routes, in_fib):
        ''' get the next hop nodes.
        if the longest prefix is coming from the current node,
        return an empty list to terminate the path searching. '''

        next_hop_nodes = []
        is_initialized = fib_routes[route_db.thisNodeName]

        lpm_route = self.get_lpm_route(route_db, dst_addr)
        if lpm_route and lpm_route.prefix.prefixLength >= cur_lpm_len:
            if in_fib and not is_initialized:
                fib_routes[route_db.thisNodeName].extend(
                    self.get_fib_path(
                        route_db.thisNodeName,
                        ipnetwork.sprint_prefix(lpm_route.prefix),
                        self.fib_agent_port,
                        self.timeout))
            min_cost = min(p.metric for p in lpm_route.paths)
            for path in [p for p in lpm_route.paths if p.metric == min_cost]:
                if len(path.nextHop.addr) == 16:
                    nh_addr = ipnetwork.sprint_addr(path.nextHop.addr)
                    next_hop_node_name = \
                        if2node[route_db.thisNodeName][(path.ifName, nh_addr)]
                    next_hop_nodes.append([
                        next_hop_node_name,
                        path.ifName,
                        path.metric,
                        nh_addr,
                    ])
        return next_hop_nodes

    def get_fib_path(self, src, dst_prefix, fib_agent_port, timeout):
        src_addr = self.get_loopback_addr(src)
        if src_addr is None:
            return []

        try:
            client = utils.get_fib_agent_client(
                src_addr, fib_agent_port, timeout)
            routes = client.getRouteTableByClient(client.client_id)
        except Exception:
            return []
        for route in routes:
            if ipnetwork.sprint_prefix(route.dest) == dst_prefix:
                return route.nexthops
        return []

    def get_paths(self, src, dst, max_hop):
        ''' calc paths from src to dst using backtracking. can add memoization to
        convert to dynamic programming for better scalability when network is large.
        '''

        dst_addr = dst
        # if dst is node, we get its loopback addr
        if ':' not in dst:
            dst_addr = self.get_loopback_addr(dst)
        try:
            ipaddress.ip_address(dst_addr)
        except ValueError:
            print("node name or ip address not valid.")
            sys.exit(1)

        adj_dbs = self.client.get_adj_dbs()
        if2node = self.get_if2node_map(adj_dbs)
        fib_routes = defaultdict(list)

        paths = []

        def _backtracking(cur, path, hop, visited, in_fib):
            if hop > max_hop:
                return

            cur_lpm_len = self.get_lpm_len_from_node(cur, dst_addr)
            next_hop_nodes = self.get_nexthop_nodes(
                self.client.get_route_db(cur),
                dst_addr,
                cur_lpm_len,
                if2node,
                fib_routes,
                in_fib)

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
                    if next_hop_node[3] == ipnetwork.sprint_addr(nexthop.addr) and\
                            next_hop_node[1] == nexthop.ifName:
                        is_nexthop_in_fib_path = True

                _backtracking(next_hop_node_name, path, hop + 1, visited,
                              is_nexthop_in_fib_path and in_fib)
                visited.remove(next_hop_node_name)
                path.pop()

        _backtracking(src, [], 1, set(src), True)
        return paths

    def print_paths(self, paths):
        if not paths:
            print("No paths are found!")
            return

        column_labels = ['Hop', 'NextHop Node', 'Interface', 'Metric', 'NextHop-v6']

        print("{} {} found.".format(len(paths),
                                    'path is' if len(paths) == 1 else 'paths are'))

        for idx, path in enumerate(paths):
            print(printing.render_horizontal_table(
                path[1], column_labels,
                caption="Path {}{}".format(idx + 1, "  *" if path[0] else ""),
                tablefmt="plain"))
            print()


class DecisionValidateCmd(DecisionCmd):
    def run(self, json):
        '''Returns a status code. 0 = success, 1 = failure'''
        print('Decision is in sync with KvStore if nothing shows up')
        print()

        decision_adj_dbs, decision_prefix_dbs, kvstore_keyvals = self.get_dbs()

        kvstore_adj_node_names = set()
        kvstore_prefix_node_names = set()

        for key, value in sorted(kvstore_keyvals.items()):
            if (key.startswith(Consts.ADJ_DB_MARKER) or
                    key.startswith(Consts.PREFIX_DB_MARKER)):
                return_code = self.print_db_delta(key, value, kvstore_adj_node_names,
                                    kvstore_prefix_node_names, decision_adj_dbs,
                                    decision_prefix_dbs, json)
                if return_code != 0:
                    return return_code

        decision_adj_node_names = {node for node in decision_adj_dbs.keys()
                                       if decision_adj_dbs[node].adjacencies}
        decision_prefix_node_names = set(decision_prefix_dbs.keys())

        if self.print_db_diff(decision_adj_node_names, kvstore_adj_node_names,
                           ['Decision', 'KvStore'], 'adj', json):
            return 1
        if self.print_db_diff(decision_prefix_node_names, kvstore_prefix_node_names,
                           ['Decision', 'KvStore'], 'prefix', json):
            return 1

        return 0

    def get_dbs(self):

        # get LSDB from Decision
        decision_adj_dbs = self.client.get_adj_dbs()
        decision_prefix_dbs = self.client.get_prefix_dbs()

        # get LSDB from KvStore
        kvstore_keyvals = utils.dump_node_kvs(self.host, self.kv_rep_port).keyVals

        return decision_adj_dbs, decision_prefix_dbs, kvstore_keyvals

    def print_db_delta(self, key, value, kvstore_adj_node_names,
                       kvstore_prefix_node_names, decision_adj_dbs,
                       decision_prefix_dbs, json):
        ''' Returns status code. 0 = success, 1 = failure'''

        if key.startswith(Consts.ADJ_DB_MARKER):
            kvstore_adj_db = deserialize_thrift_object(value.value,
                                                       lsdb_types.AdjacencyDatabase)
            node_name = kvstore_adj_db.thisNodeName
            kvstore_adj_node_names.add(node_name)
            if node_name not in decision_adj_dbs:
                print(printing.render_vertical_table(
                    [["node {}'s adj db is missing in Decision".format(node_name)]]))
                return 1
            decision_adj_db = decision_adj_dbs[node_name]

            return_code = 0
            if json:
                tags = ('in_decision', 'in_kvstore', 'changed_in_decision_and_kvstore')
                adj_list_deltas = utils.find_adj_list_deltas(
                    decision_adj_db.adjacencies, kvstore_adj_db.adjacencies, tags=tags)
                deltas_json, return_code = utils.adj_list_deltas_json(
                    adj_list_deltas, tags=tags)
                if return_code:
                    utils.print_json(deltas_json)
            else:
                lines = utils.sprint_adj_db_delta(kvstore_adj_db, decision_adj_db)
                if lines:
                    print(printing.render_vertical_table(
                          [["node {}'s adj db in Decision out of sync with KvStore's".
                            format(node_name)]]))
                    print("\n".join(lines))
                    return_code = 1

            return return_code

        if key.startswith(Consts.PREFIX_DB_MARKER):
            kvstore_prefix_db = deserialize_thrift_object(value.value,
                                                          lsdb_types.PrefixDatabase)
            node_name = kvstore_prefix_db.thisNodeName
            kvstore_prefix_node_names.add(node_name)
            if node_name not in decision_prefix_dbs:
                print(printing.render_vertical_table(
                      [["node {}'s prefix db is missing in Decision".
                        format(node_name)]]))
                return 1
            decision_prefix_db = decision_prefix_dbs[node_name]
            decision_prefix_set = {}
            utils.update_global_prefix_db(
                decision_prefix_set, decision_prefix_db)
            lines = utils.sprint_prefixes_db_delta(
                decision_prefix_set, kvstore_prefix_db)
            if lines:
                print(printing.render_vertical_table(
                      [["node {}'s prefix db in Decision out of sync with KvStore's".
                        format(node_name)]]))
                print("\n".join(lines))
                return 1

            return 0

    def print_db_diff(self, nodes_set_a, nodes_set_b, db_sources, db_type, json):
        ''' Returns a status code, 0 = success, 1 = failure'''
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
                    "nodes_down": diffs_down
                }
                return_code = 1
                utils.print_json(diffs)

        else:
            rows = []
            for node in a_minus_b:
                rows.append(["node {}'s {} db in {} but not in {}".format(
                    node, db_type, *db_sources)])
            for node in b_minus_a:
                rows.append(["node {}'s {} db in {} but not in {}".format(
                    node, db_type, *reversed(db_sources))])
            if rows:
                print(printing.render_vertical_table(rows))
                return_code = 1

        return return_code
