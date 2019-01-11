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

import bunch
import zmq
from openr.clients import prefix_mgr_client
from openr.Lsdb import ttypes as lsdb_types
from openr.PrefixManager import ttypes as prefix_mgr_types
from openr.utils import zmq_socket
from openr.utils.ipnetwork import ip_str_to_prefix, sprint_prefix


prefix_entry1 = lsdb_types.PrefixEntry(
    prefix=ip_str_to_prefix("2620:0:1cff:dead:bef1:ffff:ffff:1/128"),
    type=lsdb_types.PrefixType.LOOPBACK,
)

prefix_entry2 = lsdb_types.PrefixEntry(
    prefix=ip_str_to_prefix("2620:0:1cff:dead:bef1:ffff:ffff:2/128"),
    type=lsdb_types.PrefixType.LOOPBACK,
)

prefix_entry3 = lsdb_types.PrefixEntry(
    prefix=ip_str_to_prefix("2620:0:1cff:dead:bef1:ffff:ffff:3/128"),
    type=lsdb_types.PrefixType.LOOPBACK,
)


class PrefixMgr(object):
    def __init__(self, zmq_ctx, url):
        self._prefix_mgr_server_socket = zmq_socket.ZmqSocket(zmq_ctx, zmq.REP)
        self._prefix_mgr_server_socket.bind(url)
        self._prefix_map = {
            sprint_prefix(prefix_entry1.prefix): prefix_entry1,
            sprint_prefix(prefix_entry2.prefix): prefix_entry2,
            sprint_prefix(prefix_entry3.prefix): prefix_entry3,
        }

    def process_request(self):
        req = self._prefix_mgr_server_zmq_socket.recv_thrift_obj(
            prefix_mgr_types.PrefixManagerRequest
        )

        if req.cmd == prefix_mgr_types.PrefixManagerCommand.ADD_PREFIXES:
            for prefix_entry in req.prefixes:
                self._prefix_map[sprint_prefix(prefix_entry.prefix)] = prefix_entry
            self._prefix_mgr_server_zmq_socket.send_thrift_obj(
                prefix_mgr_types.PrefixManagerResponse(success=True)
            )

        if req.cmd == prefix_mgr_types.PrefixManagerCommand.WITHDRAW_PREFIXES:
            success = False
            for prefix_entry in req.prefixes:
                prefix_str = sprint_prefix(prefix_entry.prefix)
                if prefix_str in self._prefix_map:
                    del self._prefix_map[prefix_str]
                    success = True
            self._prefix_mgr_server_zmq_socket.send_thrift_obj(
                prefix_mgr_types.PrefixManagerResponse(success=success)
            )

        if req.cmd == prefix_mgr_types.PrefixManagerCommand.GET_ALL_PREFIXES:
            resp = prefix_mgr_types.PrefixManagerResponse()
            resp.prefixes = list(self._prefix_map.values())
            resp.success = True
            self._prefix_mgr_server_zmq_socket.send_thrift_obj(resp)


class TestPrefixMgrClient(unittest.TestCase):
    def test(self):
        zmq_socket_url = "tcp://*:5000"
        num_req = 5

        def _prefix_mgr_server():
            prefix_mgr_server = PrefixMgr(zmq.Context(), zmq_socket_url)
            for _ in range(num_req):
                prefix_mgr_server.process_request()

        def _prefix_mgr_client():
            prefix_mgr_client_inst = prefix_mgr_client.PrefixMgrClient(
                bunch.Bunch(
                    {
                        "ctx": zmq.Context(),
                        "host": "localhost",
                        "prefix_mgr_cmd_port": 5000,
                    }
                )
            )

            resp = prefix_mgr_client_inst.add_prefix(
                ["2620:0:1cff:dead:bef1:ffff:ffff:4/128"], "LOOPBACK"
            )
            self.assertTrue(resp.success)

            resp = prefix_mgr_client_inst.view_prefix()
            prefix_entry4 = lsdb_types.PrefixEntry(
                prefix=ip_str_to_prefix("2620:0:1cff:dead:bef1:ffff:ffff:4/128"),
                type=lsdb_types.PrefixType.LOOPBACK,
            )
            self.assertTrue(resp.success)
            self.assertTrue(prefix_entry4 in resp.prefixes)

            resp = prefix_mgr_client_inst.withdraw_prefix(
                ["2620:0:1cff:dead:bef1:ffff:ffff:4/128"]
            )
            self.assertTrue(resp.success)

            resp = prefix_mgr_client_inst.view_prefix()
            self.assertTrue(resp.success)
            self.assertFalse(prefix_entry4 in resp.prefixes)

            resp = prefix_mgr_client_inst.withdraw_prefix(
                ["2620:0:1cff:dead:bef1:ffff:ffff:5/128"]
            )
            self.assertFalse(resp.success)

        p = Process(target=_prefix_mgr_server)
        p.start()
        q = Process(target=_prefix_mgr_client)
        q.start()
        p.join()
        q.join()
