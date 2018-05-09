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
from builtins import range
from builtins import object

from openr.utils import socket
from openr.clients import decision_client
from openr.Decision import ttypes as decision_types
from openr.Lsdb import ttypes as lsdb_types
from openr.Fib import ttypes as fib_types

import zmq
import unittest
from multiprocessing import Process


route_db_cache = fib_types.RouteDatabase()
route_db_cache.thisNodeName = 'san jose 1'

adj_db = lsdb_types.AdjacencyDatabase()
adj_db.thisNodeName = 'san jose 1'
adj_dbs_cache = {'san jose 1': adj_db}

prefix_db = lsdb_types.PrefixDatabase()
prefix_db.thisNodeName = 'san jose 1'
prefix_dbs_cache = {'san jose 1': prefix_db}


class Decision(object):
    def __init__(self, zmq_ctx, url):
        self._decision_server_socket = socket.Socket(zmq_ctx, zmq.REP)
        self._decision_server_socket.bind(url)
        self._route_db_cache = route_db_cache
        self._adj_db_cachee = adj_dbs_cache
        self._prefix_db_cache = prefix_dbs_cache

    def _get_route_db(self, reply):

        reply.routeDb = self._route_db_cache

    def _get_adj_dbs(self, reply):

        reply.adjDbs = self._adj_db_cachee

    def _get_prefix_dbs(self, reply):

        reply.prefixDbs = self._prefix_db_cache

    def process_request(self):
        request = self._decision_server_socket.recv_thrift_obj(
            decision_types.DecisionRequest)

        reply = decision_types.DecisionReply()
        options = {decision_types.DecisionCommand.ROUTE_DB_GET: self._get_route_db,
                   decision_types.DecisionCommand.ADJ_DB_GET: self._get_adj_dbs,
                   decision_types.DecisionCommand.PREFIX_DB_GET: self._get_prefix_dbs}
        options[request.cmd](reply)
        self._decision_server_socket.send_thrift_obj(reply)


class TestDecisionClient(unittest.TestCase):
    def test(self):
        num_req = 3

        def _decision_server():
            decision_server = Decision(zmq.Context(), "tcp://*:5000")
            for _ in range(num_req):
                decision_server.process_request()

        def _decision_client():
            decision_client_inst = decision_client.DecisionClient(
                zmq.Context(), "tcp://localhost:5000")

            self.assertEqual(decision_client_inst.get_route_db(), route_db_cache)
            self.assertEqual(decision_client_inst.get_adj_dbs(), adj_dbs_cache)
            self.assertEqual(decision_client_inst.get_prefix_dbs(), prefix_dbs_cache)

        p = Process(target=_decision_server)
        p.start()
        q = Process(target=_decision_client)
        q.start()
        p.join()
        q.join()
