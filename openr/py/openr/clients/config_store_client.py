#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


from builtins import object

import zmq
from openr.clients.openr_client import OpenrClient
from openr.PersistentStore import ttypes as ps_types
from openr.utils import consts, zmq_socket


class ConfigStoreClient(OpenrClient):
    def __init__(self, cli_opts):
        super(ConfigStoreClient, self).__init__(
            None, cli_opts.config_store_url, cli_opts
        )

    def _send_req(self, req_msg):
        return self.send_and_recv_thrift_obj(req_msg, ps_types.StoreResponse)

    def erase(self, key):
        req_msg = ps_types.StoreRequest(
            requestType=ps_types.StoreRequestType.ERASE, key=key, data=""
        )
        return self._send_req(req_msg).success

    def load(self, key):
        req_msg = ps_types.StoreRequest(
            requestType=ps_types.StoreRequestType.LOAD, key=key, data=""
        )
        resp = self._send_req(req_msg)
        if resp.success:
            return resp.data
        raise KeyError("Key cannot be loaded")

    def store(self, key, data):
        req_msg = ps_types.StoreRequest(
            requestType=ps_types.StoreRequestType.STORE, key=key, data=data
        )
        return self._send_req(req_msg).success
