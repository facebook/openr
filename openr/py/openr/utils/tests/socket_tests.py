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

import unittest
import zmq
from multiprocessing import Process

from openr.Lsdb import ttypes as lsdb_types
from openr.utils import socket


class TestSocket(unittest.TestCase):

    def test_req_rep(self):
        zmq_ctx = zmq.Context()
        rep_socket = socket.Socket(zmq_ctx, zmq.REP)
        rep_socket.bind("inproc://req_rep_test")
        req_socket = socket.Socket(zmq_ctx, zmq.REQ)
        req_socket.connect("inproc://req_rep_test")

        thrift_obj = lsdb_types.PrefixDatabase()
        thrift_obj.thisNodeName = 'some node'

        req_socket.send_thrift_obj(thrift_obj)
        recv_obj = rep_socket.recv_thrift_obj(lsdb_types.PrefixDatabase)
        self.assertEqual(thrift_obj, recv_obj)
        rep_socket.send_thrift_obj(recv_obj)
        recv_obj = req_socket.recv_thrift_obj(lsdb_types.PrefixDatabase)
        self.assertEqual(thrift_obj, recv_obj)

    def test_pub_sub(self):
        zmq_ctx = zmq.Context()
        pub_socket = socket.Socket(zmq_ctx, zmq.PUB)
        pub_socket.bind("inproc://req_rep_test")
        sub_socket = socket.Socket(zmq_ctx, zmq.SUB)
        sub_socket.connect("inproc://req_rep_test")
        sub_socket.set_sock_opt(zmq.SUBSCRIBE, b"")

        thrift_obj = lsdb_types.PrefixDatabase()
        thrift_obj.thisNodeName = 'some node'

        pub_socket.send_thrift_obj(thrift_obj)
        recv_obj = sub_socket.recv_thrift_obj(lsdb_types.PrefixDatabase)
        self.assertEqual(thrift_obj, recv_obj)

    def test_dealer_dealer(self):
        zmq_ctx = zmq.Context()
        d_socket_1 = socket.Socket(zmq_ctx, zmq.DEALER)
        d_socket_1.bind("inproc://dealer_test")
        d_socket_2 = socket.Socket(zmq_ctx, zmq.DEALER)
        d_socket_2.connect("inproc://dealer_test")

        thrift_obj = lsdb_types.PrefixDatabase()
        thrift_obj.thisNodeName = 'some node'

        d_socket_1.send_thrift_obj(thrift_obj)
        recv_obj = d_socket_2.recv_thrift_obj(lsdb_types.PrefixDatabase)
        self.assertEqual(thrift_obj, recv_obj)
        d_socket_2.send_thrift_obj(recv_obj)
        recv_obj = d_socket_1.recv_thrift_obj(lsdb_types.PrefixDatabase)
        self.assertEqual(thrift_obj, recv_obj)

    def test_status_conflicts(self):
        zmq_ctx = zmq.Context()
        bind_socket = socket.Socket(zmq_ctx, zmq.REP)
        bind_socket.bind("inproc://status_test")
        with self.assertRaises(Exception):
            bind_socket.connect("inproc://status_test")

        connect_socket = socket.Socket(zmq_ctx, zmq.REP)
        connect_socket.connect("inproc://status_test")
        with self.assertRaises(Exception):
            connect_socket.bind("inproc://status_test")

    def test_in_multi_processes(self):

        thrift_obj = lsdb_types.PrefixDatabase()
        thrift_obj.thisNodeName = 'some node'

        def _send_recv():
            req_socket = socket.Socket(zmq.Context(), zmq.REQ)
            req_socket.connect("tcp://localhost:5000")
            req_socket.send_thrift_obj(thrift_obj)
            print("request sent")
            recv_obj = req_socket.recv_thrift_obj(lsdb_types.PrefixDatabase)
            print("reply received")
            self.assertEqual(thrift_obj, recv_obj)

        def _recv_send():
            rep_socket = socket.Socket(zmq.Context(), zmq.REP)
            rep_socket.bind("tcp://*:5000")
            recv_obj = rep_socket.recv_thrift_obj(lsdb_types.PrefixDatabase)
            print("request received")
            self.assertEqual(thrift_obj, recv_obj)
            rep_socket.send_thrift_obj(recv_obj)
            print("reply sent")

        q = Process(target=_recv_send)
        q.start()
        p = Process(target=_send_recv)
        p.start()
        p.join()
        q.join()
