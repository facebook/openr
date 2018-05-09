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
from openr.utils.serializer import serialize_thrift_object
from openr.clients import config_store_client
from openr.PersistentStore import ttypes as ps_types
from openr.LinkMonitor import ttypes as lm_types

import zmq
import unittest
from multiprocessing import Process

store_db = {'key1': serialize_thrift_object(lm_types.DumpLinksReply(
                                            thisNodeName='node1')),
            'key2': serialize_thrift_object(lm_types.DumpLinksReply(
                                            thisNodeName='node2'))}


class ConfigStore(object):
    def __init__(self, zmq_ctx, url):
        self._cs_server_socket = socket.Socket(zmq_ctx, zmq.REP)
        self._cs_server_socket.bind(url)
        self._store_db = store_db

    def process_request(self):
        req = self._cs_server_socket.recv_thrift_obj(ps_types.StoreRequest)

        if req.requestType == ps_types.StoreRequestType.LOAD:
            if req.key in self._store_db:
                resp = ps_types.StoreResponse(success=1, key=req.key,
                                              data=self._store_db[req.key])
            else:
                resp = ps_types.StoreResponse(success=0, key=req.key)

        if req.requestType == ps_types.StoreRequestType.ERASE:
            if req.key in self._store_db:
                resp = ps_types.StoreResponse(success=1, key=req.key)
                del store_db[req.key]
            else:
                resp = ps_types.StoreResponse(success=0, key=req.key)

        if req.requestType == ps_types.StoreRequestType.STORE:
            store_db[req.key] = req.data
            resp = ps_types.StoreResponse(success=1, key=req.key)

        self._cs_server_socket.send_thrift_obj(resp)


class TestConfigStoreClient(unittest.TestCase):
    def test(self):
        num_req = 6
        ctx = zmq.Context()

        def _cs_server():
            cs_server = ConfigStore(ctx, "inproc://openr_config_store_cmd")
            for _ in range(num_req):
                cs_server.process_request()

        def _cs_client():
            cs_client_inst = config_store_client.ConfigStoreClient(
                ctx, "inproc://openr_config_store_cmd")

            self.assertEqual(cs_client_inst.load('key1'), store_db['key1'])
            with self.assertRaises(Exception):
                cs_client_inst.load('key3')

            self.assertTrue(cs_client_inst.erase('key1'))
            with self.assertRaises(Exception):
                cs_client_inst.load('key1')

            value = serialize_thrift_object(lm_types.DumpLinksReply(
                thisNodeName='node5'))
            self.assertTrue(cs_client_inst.store('key5', value))
            self.assertEqual(cs_client_inst.load('key5'), value)

        p = Process(target=_cs_server)
        p.start()
        q = Process(target=_cs_client)
        q.start()
        p.join()
        q.join()
