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

from openr.PersistentStore import ttypes as ps_types
from openr.utils import socket, consts

import zmq


class ConfigStoreClient(object):
    def __init__(self, zmq_ctx, cs_cmd_url, timeout=consts.Consts.TIMEOUT_MS,
                 proto_factory=consts.Consts.PROTO_FACTORY):
        self._cs_cmd_socket = socket.Socket(zmq_ctx, zmq.REQ, timeout, proto_factory)
        self._cs_cmd_socket.connect(cs_cmd_url)

    def _send_req(self, req_msg):
        self._cs_cmd_socket.send_thrift_obj(req_msg)
        return self._cs_cmd_socket.recv_thrift_obj(ps_types.StoreResponse)

    def erase(self, key):
        req_msg = ps_types.StoreRequest(requestType=ps_types.StoreRequestType.ERASE,
                                        key=key,
                                        data="")
        return self._send_req(req_msg).success

    def load(self, key):
        req_msg = ps_types.StoreRequest(requestType=ps_types.StoreRequestType.LOAD,
                                        key=key,
                                        data="")
        resp = self._send_req(req_msg)
        if resp.success:
            return resp.data
        raise KeyError("Key cannot be loaded")

    def store(self, key, data):
        req_msg = ps_types.StoreRequest(requestType=ps_types.StoreRequestType.STORE,
                                        key=key,
                                        data=data)
        return self._send_req(req_msg).success
