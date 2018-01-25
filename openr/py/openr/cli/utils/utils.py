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

import bunch
import click
import copy
import datetime
import ipaddr
import json
import socket
import sys
import zmq

from itertools import product
from openr.AllocPrefix import ttypes as alloc_types
from openr.clients.kvstore_client import KvStoreClient
from openr.clients.lm_client import LMClient
from openr.IpPrefix import ttypes as ip_types
from openr.KvStore import ttypes as kv_store_types
from openr.Lsdb import ttypes as lsdb_types
from openr.Platform import FibService
from openr.Platform import ttypes as platform_types
from openr.utils import printing
from openr.utils.consts import Consts
from openr.utils.serializer import deserialize_thrift_object
from thrift.protocol import TBinaryProtocol
from thrift.transport import TSocket, TTransport


try:
    input = raw_input
except NameError:
    pass


def yesno(question):
    """
    Ask a yes/no question. No default, we want to avoid mistakes as
    much as possible. Repeat the question until we receive a valid
    answer.
    """
    while True:
        try:
            prompt = "{} [yn] ".format(question)
            answer = input(prompt)
        except EOFError:
            with open("/dev/tty") as sys.stdin:
                continue
        if answer in ['y', 'yes']:
            return True
        elif answer in ['n', 'no']:
            return False


def json_dumps(data):
    '''
    Gives consistent formatting for JSON dumps for our CLI

    :param data: python dictionary object

    :return: json encoded string
    '''

    return json.dumps(data, sort_keys=True, indent=2, ensure_ascii=False)


def time_since(timestamp):
    '''
    :param timestamp: in seconds since unix time

    :returns: difference between now and the timestamp, in a human-friendly,
              condensed format

    Example format:

    time_since(10000)
    >>> 112d11h

    :rtype: datetime.timedelta
    '''
    time_since_epoch = datetime.datetime.utcnow() - \
                       datetime.datetime(year=1970, month=1, day=1)
    tdelta = time_since_epoch - datetime.timedelta(seconds=timestamp)
    d = {"days": tdelta.days}
    d["hours"], rem = divmod(tdelta.seconds, 3600)
    d["minutes"], d["seconds"] = divmod(rem, 60)
    if (d["days"]):
        fmt = "{days}d{hours}h"
    elif (d["hours"]):
        fmt = "{hours}h{minutes}m"
    else:
        fmt = "{minutes}m{seconds}s"
    return fmt.format(**d)


def contain_any_prefix(prefix, ip_networks):
    '''
    Utility function to check if prefix contain any of the prefixes/ips

    :returns: True if prefix contains any of the ip_networks else False
    '''

    if ip_networks is None:
        return True
    prefix = ipaddr.IPNetwork(prefix)
    return any([prefix.Contains(net) for net in ip_networks])


def get_fib_agent_client(host, port, timeout_ms,
                         client_id=platform_types.FibClient.OPENR,
                         service=FibService):
    '''
    Get thrift client for talking to Fib thrift service

    :param host: thrift server name or ip
    :param port: thrift server port

    :returns: The thrift client
    :rtype: FibService.Client
    '''
    transport = TSocket.TSocket(host, port)
    transport.setTimeout(timeout_ms)
    transport = TTransport.TFramedTransport(transport)
    protocol = TBinaryProtocol.TBinaryProtocol(transport)
    client = service.Client(protocol)
    client.host = host  # Assign so that we can refer later on
    client.port = port  # Assign so that we can refer later on
    client.client_id = client_id  # Assign so that we can refer later on
    transport.open()
    return client


def get_connected_node_name(host, lm_cmd_port):
    ''' get the identity of the connected node by querying link monitor'''

    client = LMClient(
        zmq.Context(),
        'tcp://{}:{}'.format(host, lm_cmd_port))

    try:
        return client.get_identity()
    except zmq.error.Again:
        return host


def parse_nodes(host, nodes, lm_cmd_port):
    ''' parse nodes from user input

        :return set: the set of nodes
    '''

    if not nodes:
        nodes = get_connected_node_name(host, lm_cmd_port)
    nodes = set(nodes.strip().split(','))

    return nodes


def sprint_addr(addr):
    ''' binary ip addr -> string '''

    family = socket.AF_INET if len(addr) == 4 else socket.AF_INET6
    return socket.inet_ntop(family, addr)


def sprint_prefix(prefix):
    '''
    :param prefix: ip_types.IpPrefix representing an CIDR network

    :returns: string representation of prefix (CIDR network)
    :rtype: str or unicode
    '''

    return '{}/{}'.format(sprint_addr(prefix.prefixAddress.addr),
                          prefix.prefixLength)


def sprint_prefix_type(prefix_type):
    '''
    :param prefix: lsdb_types.PrefixType
    '''

    return lsdb_types.PrefixType._VALUES_TO_NAMES.get(prefix_type, None)


def sprint_prefixes_db_full(prefix_db, loopback_only=False):
    ''' given serialized prefixes output an array of lines
            representing those prefixes. IPV6 prefixes come before IPV4 prefixes.

        :prefix_db lsdb_types.PrefixDatabase: prefix database
        :loopback_only : is only loopback address expected

        :return [str]: the array of prefix strings
    '''

    prefix_strs = []
    sorted_entries = sorted(sorted(prefix_db.prefixEntries,
                                   key=lambda x: x.prefix.prefixLength),
                            key=lambda x: x.prefix.prefixAddress.addr)
    for prefix_entry in sorted_entries:
        if loopback_only and \
           prefix_entry.type is not lsdb_types.PrefixType.LOOPBACK:
            continue
        prefix_strs.append([sprint_prefix(prefix_entry.prefix),
                            sprint_prefix_type(prefix_entry.type)])

    return printing.render_horizontal_table(prefix_strs, ['Prefix', 'Type'])


def ip_str_to_addr(addr_str):
    '''
    :param addr_str: ip address in string representation

    :returns: thrift struct BinaryAddress
    :rtype: ip_types.BinaryAddress
    '''

    # Try v4
    try:
        addr = socket.inet_pton(socket.AF_INET, addr_str)
        return ip_types.BinaryAddress(addr=addr)
    except socket.error:
        pass

    # Try v6
    addr = socket.inet_pton(socket.AF_INET6, addr_str)
    return ip_types.BinaryAddress(addr=addr)


def ip_str_to_prefix(prefix_str):
    '''
    :param prefix_str: string representing a prefix (CIDR network)

    :returns: thrift struct IpPrefix
    :rtype: ip_types.IpPrefix
    '''

    ip_str, ip_len_str = prefix_str.split('/')
    return ip_types.IpPrefix(
        prefixAddress=ip_str_to_addr(ip_str),
        prefixLength=int(ip_len_str))


def alloc_prefix_to_loopback_ip_str(prefix):
    '''
    :param prefix: IpPrefix representing an allocation prefix (CIDR network)

    :returns: Loopback IP corresponding to allocation prefix
    :rtype: string
    '''

    ip_addr = prefix.prefixAddress.addr
    print(ip_addr)
    if prefix.prefixLength != 128:
        ip_addr = ip_addr[:-1] + chr(ord(ip_addr[-1]) | 1)
    print(ip_addr)
    return sprint_addr(ip_addr)


def print_prefixes_table(resp, nodes, iter_func):
    ''' print prefixes '''

    def _parse_prefixes(rows, prefix_db):
        if isinstance(prefix_db, kv_store_types.Value):
            prefix_db = deserialize_thrift_object(prefix_db.value,
                                                  lsdb_types.PrefixDatabase)

        rows.append(["{}'s prefixes".format(prefix_db.thisNodeName),
                     sprint_prefixes_db_full(prefix_db)])

    rows = []
    iter_func(rows, resp, nodes, _parse_prefixes)
    print(printing.render_vertical_table(rows))


def thrift_to_dict(thrift_inst, update_func=None):
    ''' convert thrift instance into a dict in strings

        :param thrift_inst: a thrift instance
        :param update_func: transformation function to update dict value of
                            thrift object. It is optional.

        :return dict: dict with attributes as key, value in strings
    '''

    gen_dict = copy.copy(thrift_inst).__dict__
    if update_func is not None:
        update_func(gen_dict, thrift_inst)

    return gen_dict


def prefix_entry_to_dict(prefix_entry):
    ''' convert prefixEntry from thrift instance into a dict in strings '''

    def _update(prefix_entry_dict, prefix_entry):
        # Only addrs need string conversion so we udpate them
        prefix_entry_dict.update({
            'prefix': sprint_prefix(prefix_entry.prefix),
        })

    return thrift_to_dict(prefix_entry, _update)


def print_prefixes_json(resp, nodes, iter_func):
    ''' print prefixes in json '''

    def _parse_prefixes(prefixes_map, prefix_db):
        if isinstance(prefix_db, kv_store_types.Value):
            prefix_db = deserialize_thrift_object(prefix_db.value,
                                                  lsdb_types.PrefixDatabase)

        prefixEntries = map(prefix_entry_to_dict, prefix_db.prefixEntries)
        prefixes_map[prefix_db.thisNodeName] = {'prefixEntries': prefixEntries}

    prefixes_map = {}
    iter_func(prefixes_map, resp, nodes, _parse_prefixes)
    print(json_dumps(prefixes_map))


def update_global_adj_db(global_adj_db, adj_db):
    ''' update the global adj map based on publication from single node

        :param global_adj_map map(node, AdjacencyDatabase)
            the map for all adjacencies in the network - to be updated
        :param adj_db lsdb_types.AdjacencyDatabase: publication from single
            node
    '''

    assert(isinstance(adj_db, lsdb_types.AdjacencyDatabase))

    global_adj_db[adj_db.thisNodeName] = adj_db


def build_global_adj_db(resp):
    ''' build a map of all adjacencies in the network. this is used
        for bi-directional validation

        :param resp kv_store_types.Publication: the parsed publication

        :return map(node, AdjacencyDatabase): the global
            adj map, devices name mapped to devices it connects to, and
            properties of that connection
    '''

    # map: (node) -> AdjacencyDatabase)
    global_adj_db = {}

    for (key, value) in resp.keyVals.items():
        if not key.startswith(Consts.ADJ_DB_MARKER):
            continue
        adj_db = deserialize_thrift_object(
            value.value, lsdb_types.AdjacencyDatabase)
        update_global_adj_db(global_adj_db, adj_db)

    return global_adj_db


def build_global_prefix_db(resp):
    ''' build a map of all prefixes in the network. this is used
        for checking for changes in topology

        :param resp kv_store_types.Publication: the parsed publication

        :return map(node, set([prefix])): the global prefix map,
            prefixes mapped to the node
    '''

    # map: (node) -> set([prefix])
    global_prefix_db = {}

    for (key, value) in resp.keyVals.items():
        if not key.startswith(Consts.PREFIX_DB_MARKER):
            continue
        prefix_db = deserialize_thrift_object(value.value,
                                              lsdb_types.PrefixDatabase)
        update_global_prefix_db(global_prefix_db, prefix_db)

    return global_prefix_db


def build_global_interface_db(resp):
    '''
    build a map<node-name, InterfaceDatabase.bunch> which is used for tracking
    changes in interface database of node

    :param resp kv_store_types.Publication: the parsed publication

    :return map<node, InterfaceDatabase.bunch>
    '''

    global_intf_db = {}
    for (key, value) in resp.keyVals.items():
        if not key.startswith(Consts.INTERFACE_DB_MARKER):
            continue
        intf_db = interface_db_to_dict(value)
        global_intf_db[intf_db.thisNodeName] = intf_db
    return global_intf_db


def dump_adj_db_full(global_adj_db, adj_db, bidir):
    ''' given an adjacency database, dump neighbors. Use the
            global adj database to validate bi-dir adjacencies

        :param global_adj_db map(str, AdjacencyDatabase):
            map of node names to their adjacent node names
        :param adj_db lsdb_types.AdjacencyDatabase: latest from kv store
        :param bidir bool: only dump bidir adjacencies

        :return (nodeLabel, [adjacencies]): tuple of node label and list
            of adjacencies
    '''

    assert(isinstance(adj_db, lsdb_types.AdjacencyDatabase))
    this_node_name = adj_db.thisNodeName

    if not bidir:
        return (adj_db.nodeLabel, adj_db.isOverloaded, adj_db.adjacencies)

    adjacencies = []

    for adj in adj_db.adjacencies:
        other_node_db = global_adj_db.get(adj.otherNodeName, None)
        if other_node_db is None:
            continue
        other_node_neighbors = set(a.otherNodeName for a in other_node_db.adjacencies)
        if this_node_name not in other_node_neighbors:
            continue
        adjacencies.append(adj)

    return (adj_db.nodeLabel, adj_db.isOverloaded, adjacencies)


def adj_to_dict(adj):
    ''' convert adjacency from thrift instance into a dict in strings '''

    def _update(adj_dict, adj):
        # Only addrs need string conversion so we udpate them
        adj_dict.update({
            'nextHopV6': sprint_addr(adj.nextHopV6.addr),
            'nextHopV4': sprint_addr(adj.nextHopV4.addr)
        })

    return thrift_to_dict(adj, _update)


def adj_db_to_dict(adjs_map, adj_dbs, adj_db, bidir, version):
    ''' convert adj db to dict '''

    node_label, is_overloaded, adjacencies = dump_adj_db_full(
        adj_dbs, adj_db, bidir)

    if not adjacencies:
        return

    adjacencies = map(adj_to_dict, adjacencies)

    # Dump is keyed by node name with attrs as key values
    adjs_map[adj_db.thisNodeName] = {
        'node_label': node_label,
        'overloaded': is_overloaded,
        'adjacencies': adjacencies
    }
    if version:
        adjs_map[adj_db.thisNodeName]['version'] = version


def adj_dbs_to_dict(resp, nodes, bidir, iter_func):
    ''' get parsed adjacency db

        :param resp kv_store_types.Publication, or decision_types.adjDbs
        :param nodes set: the set of the nodes to print prefixes for
        :param bidir bool: only dump bidirectional adjacencies

        :return map(node, map(adjacency_keys, (adjacency_values)): the parsed
            adjacency DB in a map with keys and values in strings
    '''
    adj_dbs = resp
    if isinstance(adj_dbs, kv_store_types.Publication):
        adj_dbs = build_global_adj_db(resp)

    def _parse_adj(adjs_map, adj_db):
        version = None
        if isinstance(adj_db, kv_store_types.Value):
            version = adj_db.version
            adj_db = deserialize_thrift_object(adj_db.value,
                                               lsdb_types.AdjacencyDatabase)
        adj_db_to_dict(adjs_map, adj_dbs, adj_db, bidir, version)

    adjs_map = {}
    iter_func(adjs_map, resp, nodes, _parse_adj)
    return adjs_map


def print_adjs_json(adjs_map):
    ''' print adjacencies in json

        :param adjacencies as list of dict
    '''

    print(json_dumps(adjs_map))


def print_adjs_table(adjs_map, enable_color):
    ''' print adjacencies

        :param adjacencies as list of dict
    '''

    column_labels = ['Neighbor', 'Local Interface', 'Remote Interface',
                     'Metric', 'Weight', 'Adj Label', 'NextHop-v4',
                     'NextHop-v6', 'Uptime']

    output = []
    for node, val in sorted(adjs_map.items()):
        # report overloaded status in color
        is_overloaded = val['overloaded']
        # report overloaded status in color
        overload_color = 'red' if is_overloaded else 'green'
        overload_status = click.style(
            '{}'.format(is_overloaded), fg=overload_color)
        cap = "{}'s adjacencies, version: {}, Node Label: {}, " "Overloaded?: {}".format(
            node, val['version']
            if 'version' in val else 'N/A', val['node_label'], overload_status
            if enable_color else ('TRUE' if is_overloaded else 'FALSE')
        )

        # horizontal adj table for a node
        rows = []
        for adj in sorted(val['adjacencies'], key=lambda adj: adj['otherNodeName']):
            overload_status = click.style('Overloaded', fg='red')
            metric = (overload_status if enable_color else
                      'OVERLOADED') if adj['isOverloaded'] else adj['metric']
            uptime = time_since(adj['timestamp']) if adj['timestamp'] else ''

            rows.append([adj['otherNodeName'], adj['ifName'],
                         adj['otherIfName'], metric,
                         adj['weight'], adj['adjLabel'], adj['nextHopV4'],
                         adj['nextHopV6'], uptime])
            seg = printing.render_horizontal_table(rows, column_labels,
                                                   tablefmt='plain')
        output.append([cap, seg])

    print(printing.render_vertical_table(output))


def sprint_adj_db_full(global_adj_db, adj_db, bidir):
    ''' given serialized adjacency database, print neighbors. Use the
            global adj database to validate bi-dir adjacencies

        :param global_adj_db map(str, AdjacencyDatabase):
            map of node names to their adjacent node names
        :param adj_db lsdb_types.AdjacencyDatabase: latest from kv store
        :param bidir bool: only print bidir adjacencies

        :return [str]: list of string to be printed
    '''

    assert(isinstance(adj_db, lsdb_types.AdjacencyDatabase))
    this_node_name = adj_db.thisNodeName
    node_label_str = 'Node Label: {}'.format(adj_db.nodeLabel)

    rows = []

    column_labels = ['Neighbor', 'Local Interface', 'Remote Interface',
                     'Metric', 'Adj Label', 'NextHop-v4', 'NextHop-v6',
                     'Uptime']

    for adj in adj_db.adjacencies:
        if bidir:
            other_node_db = global_adj_db.get(adj.otherNodeName, None)
            if other_node_db is None:
                continue
            other_node_neighbors = set(a.otherNodeName for a in
                                       other_node_db.adjacencies)
            if this_node_name not in other_node_neighbors:
                continue

        nh_v6 = sprint_addr(adj.nextHopV6.addr)
        nh_v4 = sprint_addr(adj.nextHopV4.addr)
        overload_status = click.style('Overloaded', fg='red')
        metric = overload_status if adj.isOverloaded else adj.metric
        uptime = time_since(adj.timestamp) if adj.timestamp else ''

        rows.append([adj.otherNodeName, adj.ifName, adj.otherIfName,
                     metric, adj.adjLabel, nh_v4, nh_v6, uptime])

    return node_label_str, printing.render_horizontal_table(rows, column_labels)


def interface_db_to_dict(value):
    '''
    Convert a thrift::Value representation of InterfaceDatabase to bunch
    object
    '''

    def _parse_intf_info(info):
        return bunch.Bunch(**{
            'isUp': info.isUp,
            'ifIndex': info.ifIndex,
            'v4Addrs': [sprint_addr(v.addr) for v in info.v4Addrs],
            'v6Addrs': [sprint_addr(v.addr) for v in info.v6LinkLocalAddrs],
        })

    assert(isinstance(value, kv_store_types.Value))
    intf_db = deserialize_thrift_object(value.value,
                                        lsdb_types.InterfaceDatabase)
    return bunch.Bunch(**{
        'thisNodeName': intf_db.thisNodeName,
        'interfaces': {k: _parse_intf_info(v)
                       for k, v in intf_db.interfaces.items()},
    })


def interface_dbs_to_dict(publication, nodes, iter_func):
    ''' get parsed interface dbs

        :param publication kv_store_types.Publication
        :param nodes set: the set of the nodes to filter interfaces for

        :return map(node, InterfaceDatabase.bunch): the parsed
            adjacency DB in a map with keys and values in strings
    '''

    assert(isinstance(publication, kv_store_types.Publication))

    def _parse_intf_db(intf_map, value):
        intf_db = interface_db_to_dict(value)
        intf_map[intf_db.thisNodeName] = intf_db

    intf_dbs_map = {}
    iter_func(intf_dbs_map, publication, nodes, _parse_intf_db)
    return intf_dbs_map


def sprint_interface_table(intf_db, print_all):
    '''
    @param intf_db: InterfaceDatabase.bunch
    '''

    columns = ['Interface', 'Status', 'ifIndex', 'Addresses']
    rows = []
    for intf_name, intf in sorted(intf_db.interfaces.items()):
        if not (intf.isUp or print_all):
            continue
        status = 'UP' if intf.isUp else 'DOWN'
        rows.append([intf_name, status, intf.ifIndex, ''])

        first_row = True
        for addr in intf.v4Addrs + intf.v6Addrs:
            if first_row:
                first_row = False
                rows[-1][-1] = addr
            else:
                rows.append(['', '', '', addr])

    return printing.render_horizontal_table(rows, columns, None)


def print_interfaces_table(intf_map, print_all):
    '''
    @param intf_db: map<node-name, InterfaceDatabase.bunch>
    '''

    lines = []
    for intf_db in sorted(intf_map.values(), key=lambda x: x.thisNodeName):
        lines.append('> {}\'s interfaces'.format(intf_db.thisNodeName))
        lines.append(sprint_interface_table(intf_db, print_all))
        lines.append('')
    lines.pop()
    print('\n'.join(lines))


def print_routes_table(route_db, prefixes=None):
    ''' print the the routes from Decision/Fib module '''

    networks = None
    if prefixes:
        networks = [ipaddr.IPNetwork(p) for p in prefixes]

    route_strs = []
    for route in sorted(route_db.routes, key=lambda x: x.prefix.prefixAddress.addr):
        prefix_str = sprint_prefix(route.prefix)
        if not contain_any_prefix(prefix_str, networks):
            continue

        paths_str = '\n'.join(["via {}@{} metric {}".format(
            sprint_addr(path.nextHop.addr),
            path.ifName, path.metric) for path in route.paths])
        route_strs.append((prefix_str, paths_str))

    caption = "Routes for {}".format(route_db.thisNodeName)
    if not route_strs:
        route_strs.append(['No routes found.'])
    print(printing.render_vertical_table(route_strs, caption=caption))


def path_to_dict(path):
    ''' convert path from thrift instance into a dict in strings '''

    def _update(path_dict, path):
        path_dict.update({
            'nextHop': sprint_addr(path.nextHop.addr),
        })

    return thrift_to_dict(path, _update)


def route_to_dict(route):
    ''' convert route from thrift instance into a dict in strings '''

    def _update(route_dict, route):
        route_dict.update({
            'prefix': sprint_prefix(route.prefix),
            'paths': map(path_to_dict, route.paths)
        })

    return thrift_to_dict(route, _update)


def route_db_to_dict(route_db):
    ''' convert route from thrift instance into a dict in strings '''

    return {'routes': map(route_to_dict, route_db.routes)}


def print_routes_json(route_db_dict, prefixes=None):

    networks = None
    if prefixes:
        networks = [ipaddr.IPNetwork(p) for p in prefixes]

    # Filter out all routes based on prefixes!
    for routes in route_db_dict.values():
        filtered_routes = []
        for route in routes["routes"]:
            if not contain_any_prefix(route["prefix"], networks):
                continue
            filtered_routes.append(route)
        routes["routes"] = filtered_routes

    print(json_dumps(route_db_dict))


def find_adj_list_deltas(old_adj_list, new_adj_list):
    ''' given the old adj list and the new one for some node, return
        change list.

        :param old_adj_list [Adjacency]: old adjacency list
        :param new_adj_list [Adjacency]: new adjacency list

        :return [(str, Adjacency, Adjacency)]: list of tuples of
            (changeType, oldAdjacency, newAdjacency)
            in the case where an adjacency is added or removed,
            oldAdjacency or newAdjacency is None, respectively
    '''

    old_neighbors = set(a.otherNodeName for a in old_adj_list)
    new_neighbors = set(a.otherNodeName for a in new_adj_list)
    delta_list = [("NEIGHBOR_DOWN", a, None) for a in old_adj_list
                  if a.otherNodeName in old_neighbors - new_neighbors]
    delta_list.extend([("NEIGHBOR_UP", None, a) for a in new_adj_list
                       if a.otherNodeName in new_neighbors - old_neighbors])
    delta_list.extend([("NEIGHBOR_UPDATE", a, b) for a, b
                      in product(old_adj_list, new_adj_list)
                      if (a.otherNodeName == b.otherNodeName and
                          a.ifName == b.ifName and
                          a.otherNodeName in new_neighbors & old_neighbors and
                          a != b)])
    return delta_list


def adjacency_to_dict(adjacency):
    ''' convert adjacency from thrift instance into a dict in strings

        :param adjacency as a thrift instance: adjacency

        :return dict: dict with adjacency attributes as key, value in strings
    '''

    # Only addrs need string conversion so we udpate them
    adj_dict = copy.copy(adjacency).__dict__
    adj_dict.update({
        'nextHopV6': sprint_addr(adjacency.nextHopV6.addr),
        'nextHopV4': sprint_addr(adjacency.nextHopV4.addr)
    })

    return adj_dict


def sprint_adj_delta(old_adj, new_adj):
    ''' given old and new adjacency, create a list of strings that summarize
        changes. If oldAdj is None, this function prints all attridutes of
        newAdj

        :param oldAdj Adjacency: can be None
        :param newAdj Adjacency: new

        :return str: table summarizing the change
    '''
    assert new_adj is not None
    rows = []
    new_adj_dict = adjacency_to_dict(new_adj)
    if old_adj is not None:
        old_adj_dict = adjacency_to_dict(old_adj)
        for k in sorted(new_adj_dict.keys()):
            if old_adj_dict.get(k) != new_adj_dict.get(k):
                rows.append([k, old_adj_dict.get(k), "-->",
                            new_adj_dict.get(k)])
    else:
        for k in sorted(new_adj_dict.keys()):
            rows.append([k, new_adj_dict[k]])
    return printing.render_horizontal_table(rows)


def sprint_pub_update(global_publication_db, key, value):
    '''
    store new version and originatorId for a key in the global_publication_db
    return a string summarizing any changes in a publication from kv store
    '''

    rows = []
    old_version, old_originator_id = global_publication_db.get(
        key, (None, None))

    if old_version != value.version:
        rows.append(["version:", old_version, "-->", value.version])
    if old_originator_id != value.originatorId:
        rows.append(["originatorId:", old_originator_id, "-->",
                    value.originatorId])
    ttl = 'INF' if value.ttl == Consts.CONST_TTL_INF else value.ttl
    rows.append(["ttlVersion:", "", "-->", value.ttlVersion])
    rows.append(["ttl:", "", "-->", ttl])
    global_publication_db[key] = (value.version, value.originatorId)
    return printing.render_horizontal_table(rows, tablefmt="plain") if rows else ""


def update_global_prefix_db(global_prefix_db, prefix_db):
    ''' update the global prefix map with a single publication

        :param global_prefix_map map(node, set([str])): map of all prefixes
            in the network
        :param prefix_db lsdb_types.PrefixDatabase: publication from single
            node
    '''

    assert(isinstance(prefix_db, lsdb_types.PrefixDatabase))

    this_node = prefix_db.thisNodeName

    prefix_set = set([])
    for prefix_entry in prefix_db.prefixEntries:
        addr_str = sprint_addr(prefix_entry.prefix.prefixAddress.addr)
        prefix_len = prefix_entry.prefix.prefixLength
        prefix_set.add("{}/{}".format(addr_str, prefix_len))

    global_prefix_db[this_node] = prefix_set

    return


def sprint_adj_db_delta(new_adj_db, old_adj_db):
    ''' given serialized adjacency database, print neighbors delta as
            compared to the supplied global state

        :param new_adj_db lsdb_types.AdjacencyDatabase: latest from kv store
        :param old_adj_db lsdb_types.AdjacencyDatabase: last one we had

        :return [str]: list of string to be printed
    '''

    # check for deltas between old and new
    # first check for changes in the adjacencies lists
    adj_list_deltas = find_adj_list_deltas(old_adj_db.adjacencies,
                                           new_adj_db.adjacencies)

    strs = []

    for change_type, old_adj, new_adj in adj_list_deltas:
        if change_type == "NEIGHBOR_DOWN":
            strs.append("{}: {} via {}"
                        .format(change_type, old_adj.otherNodeName,
                                old_adj.ifName))
        if change_type == "NEIGHBOR_UP" or change_type == "NEIGHBOR_UPDATE":
            strs.append("{}: {} via {}\n{}"
                        .format(change_type, new_adj.otherNodeName,
                                new_adj.ifName,
                                sprint_adj_delta(old_adj, new_adj)))

    # check for other adjDB changes
    old_db_dict = copy.copy(old_adj_db).__dict__
    old_db_dict.pop('adjacencies', None)
    old_db_dict.pop('perfEvents', None)
    new_db_dict = copy.copy(new_adj_db).__dict__
    new_db_dict.pop('adjacencies', None)
    new_db_dict.pop('perfEvents', None)
    if new_db_dict != old_db_dict:
        rows = []
        strs.append("ADJ_DB_UPDATE: {}".format(new_adj_db.thisNodeName))
        for k in sorted(new_db_dict.keys()):
            if old_db_dict.get(k) != new_db_dict.get(k):
                rows.append([k, old_db_dict.get(k), "-->", new_db_dict.get(k)])
        strs.append(printing.render_horizontal_table(rows, tablefmt='plain'))

    return strs


def sprint_interface_db_delta(new_intf_db, old_intf_db):
    '''
    Print delta between new and old interface db

    @param new_intf_db: InterfaceDatabase.bunch
    @param old_intf_db: InterfaceDatabase.bunch
    '''

    assert(new_intf_db is not None)
    assert(old_intf_db is not None)

    new_intfs = set(new_intf_db.interfaces.keys())
    old_intfs = set(old_intf_db.interfaces.keys())

    added_intfs = new_intfs - old_intfs
    removed_intfs = old_intfs - new_intfs
    updated_intfs = new_intfs & old_intfs
    lines = []

    for intf_name in added_intfs:
        lines.append('INTERFACE_ADDED: {}\n'.format(intf_name))
        intf = new_intf_db.interfaces[intf_name]
        rows = []
        for k in sorted(intf.keys()):
            rows.append([k, "", "-->", intf.get(k)])
        lines.append(printing.render_horizontal_table(rows, tablefmt='plain'))

    for intf_name in removed_intfs:
        lines.append('INTERFACE_REMOVED: {}\n'.format(intf_name))

    for intf_name in updated_intfs:
        new_intf = new_intf_db.interfaces[intf_name]
        old_intf = old_intf_db.interfaces[intf_name]
        if new_intf == old_intf:
            continue
        lines.append('INTERFACE_UPDATED: {}'.format(intf_name))
        rows = []
        for k in sorted(new_intf.keys()):
            if old_intf.get(k) != new_intf.get(k):
                rows.append([k, old_intf.get(k), "-->", new_intf.get(k)])
        lines.append(printing.render_horizontal_table(rows, tablefmt='plain'))

    return lines


def sprint_prefixes_db_delta(global_prefixes_db, prefix_db):
    ''' given serialzied prefixes for a single node, output the delta
            between those prefixes and global prefixes snapshot

        :global_prefixes_db map(node, set([str])): global prefixes
        :prefix_db lsdb_types.PrefixDatabase: latest from kv store

        :return [str]: the array of prefix strings
    '''

    this_node_name = prefix_db.thisNodeName
    prev_prefixes = global_prefixes_db.get(this_node_name, set([]))

    cur_prefixes = set([])
    for prefix_entry in prefix_db.prefixEntries:
        cur_prefixes.add(sprint_prefix(prefix_entry.prefix))

    added_prefixes = cur_prefixes - prev_prefixes
    removed_prefixes = prev_prefixes - cur_prefixes

    strs = ["+ {}".format(prefix) for prefix in added_prefixes]
    strs.extend(["- {}".format(prefix) for prefix in removed_prefixes])

    return strs


def dump_node_kvs(node, kv_rep_port):
    client = KvStoreClient(zmq.Context(),
                           "tcp://[{}]:{}".format(node, kv_rep_port))
    try:
        kv = client.dump_all_with_prefix()
    except zmq.error.Again:
        print('cannot connect to {}\'s kvstore'.format(node))
        return None
    return kv


def print_allocations_table(alloc_str):
    ''' print static allocations '''

    rows = []
    allocations = deserialize_thrift_object(
        alloc_str, alloc_types.StaticAllocation)
    for node, prefix in allocations.nodePrefixes.items():
        rows.append([node, sprint_prefix(prefix)])
    print(printing.render_horizontal_table(rows, ['Node', 'Prefix']))
