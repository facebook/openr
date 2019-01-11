#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import, division, print_function, unicode_literals

import unittest
from builtins import object, range
from multiprocessing import Process

import zmq
from openr.clients import fib_client
from openr.Fib import ttypes as fib_types
from openr.utils import zmq_socket


route_db_cache = fib_types.RouteDatabase()
route_db_cache.thisNodeName = "san jose 1"


class Fib(object):
    def __init__(self, zmq_ctx, url):
        self._fib_server_socket = zmq_socket.ZmqSocket(zmq_ctx, zmq.REP)
        self._fib_server_socket.bind(url)
        self._route_db_cache = route_db_cache

    def process_request(self):
        self._fib_server_socket.recv_thrift_obj(fib_types.FibRequest)
        self._fib_server_socket.send_thrift_obj(self._route_db_cache)


class TestFibClient(unittest.TestCase):
    def test(self):
        num_req = 1

        def _fib_server():
            fib_server = Fib(zmq.Context(), "tcp://*:5000")
            for _ in range(num_req):
                fib_server.process_request()

        def _fib_client():
            fib_client_inst = fib_client.FibClient(
                zmq.Context(), "tcp://localhost:5000"
            )
            self.assertEqual(fib_client_inst.get_route_db(), route_db_cache)

        p = Process(target=_fib_server)
        p.start()
        q = Process(target=_fib_client)
        q.start()
        p.join()
        q.join()
