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
from openr.clients import monitor_client
from fbzmq.Monitor import ttypes as monitor_types

import zmq
import unittest
from multiprocessing import Process


monitor_cache = monitor_types.CounterValuesResponse()
monitor_cache.counters = {'san jose': monitor_types.Counter(value=3.5)}


class Monitor(object):
    def __init__(self, zmq_ctx, url):
        self._monitor_server_socket = socket.Socket(zmq_ctx, zmq.DEALER)
        self._monitor_server_socket.bind(url)
        self._monitor_cache = monitor_cache

    def process_request(self):
        request = self._monitor_server_socket.recv_thrift_obj(
            monitor_types.MonitorRequest)
        if request.cmd == monitor_types.MonitorCommand.DUMP_ALL_COUNTER_DATA:
            self._monitor_server_socket.send_thrift_obj(self._monitor_cache)


class TestMonitorClient(unittest.TestCase):
    def test(self):
        num_req = 1

        def _monitor_server():
            monitor_server = Monitor(zmq.Context(), "tcp://*:5000")
            for _ in range(num_req):
                monitor_server.process_request()

        def _monitor_client():
            monitor_client_inst = monitor_client.MonitorClient(
                zmq.Context(), "tcp://localhost:5000")
            self.assertEqual(monitor_client_inst.dump_all_counter_data(), monitor_cache)

        p = Process(target=_monitor_server)
        p.start()
        q = Process(target=_monitor_client)
        q.start()
        p.join()
        q.join()
