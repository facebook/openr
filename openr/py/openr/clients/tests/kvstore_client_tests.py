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
from openr.clients import kvstore_client
from openr.KvStore import ttypes as kv_store_types

import zmq
import unittest
from multiprocessing import Process


value1 = kv_store_types.Value()
value1.originatorId = 'san jose 1'

value2 = kv_store_types.Value()
value2.originatorId = 'san jose 2'

value3 = kv_store_types.Value()
value3.originatorId = 'san jose 3'

value4 = kv_store_types.Value()
value4.originatorId = 'san jose 4'

value5 = kv_store_types.Value()
value5.originatorId = 'san francisco 1'

kv_store_cache = {'san jose 1': value1, 'san jose 2': value2,
                  'san jose 3': value3, 'san jose 4': value4,
                  'san francisco 1': value5}


class KVStore(object):
    def __init__(self, zmq_ctx, url):
        self._kv_store_server_socket = socket.Socket(zmq_ctx, zmq.REP)
        self._kv_store_server_socket.bind(url)
        self._kv_store = kv_store_cache

    def _get_keys(self, request):
        keys = request.keyGetParams.keys
        publication = kv_store_types.Publication({})
        for key in keys:
            if key in self._kv_store:
                publication.keyVals[key] = self._kv_store[key]
        return publication

    def _dump_all_with_prefix(self, request):
        prefix = request.keyDumpParams.prefix
        publication = kv_store_types.Publication({})

        for key in self._kv_store:
            if key.startswith(prefix):
                publication.keyVals[key] = self._kv_store[key]
        return publication

    def process_request(self):
        request = self._kv_store_server_socket.recv_thrift_obj(kv_store_types.Request)
        options = {kv_store_types.Command.KEY_GET: self._get_keys,
                   kv_store_types.Command.KEY_DUMP: self._dump_all_with_prefix}
        publication = options[request.cmd](request)
        self._kv_store_server_socket.send_thrift_obj(publication)


class TestKVStoreClient(unittest.TestCase):

    def test(self):
        num_req = 5

        def _kv_store_server():
            kv_store_server = KVStore(zmq.Context(), "tcp://*:5000")
            for _ in range(num_req):
                kv_store_server.process_request()

        def _kv_store_client():
            kv_store_client_inst = kvstore_client.KvStoreClient(
                zmq.Context(), "tcp://localhost:5000")

            publication = kv_store_client_inst.get_keys(
                ['san jose 1', 'san francisco 1', 'virginia'])
            key_values = publication.keyVals
            self.assertEqual(
                key_values, {'san jose 1': value1, 'san francisco 1': value5})

            publication = kv_store_client_inst.dump_all_with_prefix('san jose 3')
            key_values = publication.keyVals
            self.assertEqual(key_values, {'san jose 3': value3})

            publication = kv_store_client_inst.dump_all_with_prefix('san jose')
            key_values = publication.keyVals
            self.assertEqual(len(key_values), 4)

            publication = kv_store_client_inst.dump_all_with_prefix('')
            key_values = publication.keyVals
            self.assertEqual(len(key_values), 5)

            publication = kv_store_client_inst.dump_all_with_prefix('virginia')
            key_values = publication.keyVals
            self.assertEqual(len(key_values), 0)

        p = Process(target=_kv_store_server)
        p.start()
        q = Process(target=_kv_store_client)
        q.start()
        p.join()
        q.join()
