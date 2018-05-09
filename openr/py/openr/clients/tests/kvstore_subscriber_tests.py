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

from openr.utils import socket
from openr.clients import kvstore_subscriber
from openr.KvStore import ttypes as kv_store_types

import zmq
import unittest
import time
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
publication = kv_store_types.Publication(kv_store_cache)


class KvStorePub(object):
    def __init__(self, zmq_ctx, url):
        self._kv_store_publisher_socket = socket.Socket(zmq_ctx, zmq.PUB)
        self._kv_store_publisher_socket.bind(url)

    def publish(self):
        self._kv_store_publisher_socket.send_thrift_obj(publication)


class TestKVStoreSubscriberClient(unittest.TestCase):

    def test(self):
        runtime_second = 1

        def _kv_store_server():
            kvstore_pub_inst = KvStorePub(zmq.Context(), "tcp://*:5000")
            while True:
                kvstore_pub_inst.publish()
                time.sleep(0.2)

        def _kv_store_client():
            kvstore_sub_inst = kvstore_subscriber.KvStoreSubscriber(
                zmq.Context(), "tcp://localhost:5000")
            while True:
                resp = kvstore_sub_inst.listen()
                self.assertEqual(resp, publication)

        p = Process(target=_kv_store_server)
        p.start()
        q = Process(target=_kv_store_client)
        q.start()
        time.sleep(runtime_second)
        p.terminate()
        q.terminate()
        p.join()
        q.join()
