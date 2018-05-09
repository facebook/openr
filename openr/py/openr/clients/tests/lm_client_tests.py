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
from openr.clients import lm_client
from openr.LinkMonitor import ttypes as lm_types

import zmq
import unittest
from multiprocessing import Process


dump_links_cache = lm_types.DumpLinksReply()
dump_links_cache.thisNodeName = 'san jose 1'


class LM(object):
    def __init__(self, zmq_ctx, url):
        self._lm_server_socket = socket.Socket(zmq_ctx, zmq.DEALER)
        self._lm_server_socket.bind(url)
        self._dump_links_cache = dump_links_cache

    def _dump_links(self, request):

        return self._dump_links_cache

    def process_request(self):
        request = self._lm_server_socket.recv_thrift_obj(lm_types.LinkMonitorRequest)

        # more options will be covered in the future
        options = {lm_types.LinkMonitorCommand.DUMP_LINKS: self._dump_links}
        reply = options[request.cmd](request)

        self._lm_server_socket.send_thrift_obj(reply)


class TestLMClient(unittest.TestCase):
    def test(self):
        num_req = 1

        def _lm_server():
            lm_server = LM(zmq.Context(), "tcp://*:5000")
            for _ in range(num_req):
                lm_server.process_request()

        def _lm_client():
            lm_client_inst = lm_client.LMClient(
                zmq.Context(), "tcp://localhost:5000")
            self.assertEqual(lm_client_inst.dump_links(), dump_links_cache)

        p = Process(target=_lm_server)
        p.start()
        q = Process(target=_lm_client)
        q.start()
        p.join()
        q.join()
