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
from builtins import str
from builtins import object

from openr.KvStore import ttypes as kv_store_types
from openr.utils import consts, serializer, socket

import zmq


class KvStoreClient(object):
    def __init__(self, zmq_ctx, kv_store_cmd_url, timeout=consts.Consts.TIMEOUT_MS,
                 proto_factory=consts.Consts.PROTO_FACTORY):
        self._kv_store_cmd_socket = socket.Socket(zmq_ctx, zmq.REQ, timeout,
                                                  proto_factory)
        self._kv_store_cmd_socket.connect(kv_store_cmd_url)

    def get_keys(self, keys):
        ''' Get values corresponding to keys from KvStore.
            It gets from local snapshot KeyVals of the kvstore.
        '''

        req_msg = kv_store_types.Request(kv_store_types.Command.KEY_GET)
        req_msg.keyGetParams = kv_store_types.KeyGetParams(keys)
        self._kv_store_cmd_socket.send_thrift_obj(req_msg)

        return self._kv_store_cmd_socket.recv_thrift_obj(
            kv_store_types.Publication)

    def set_key(self, keyVals):

        req_msg = kv_store_types.Request(kv_store_types.Command.KEY_SET)
        req_msg.keySetParams = kv_store_types.KeySetParams(keyVals)
        self._kv_store_cmd_socket.send_thrift_obj(req_msg)

        return self._kv_store_cmd_socket.recv()

    def dump_all_with_prefix(self, prefix="", originator=""):
        '''  dump the entries of kvstore whose key matches the given prefix
             if prefix is an empty string, the full KV store is dumped
        '''

        req_msg = kv_store_types.Request(kv_store_types.Command.KEY_DUMP)
        req_msg.keyDumpParams = kv_store_types.KeyDumpParams(prefix)
        req_msg.keyDumpParams.originator = {originator}
        self._kv_store_cmd_socket.send_thrift_obj(req_msg)

        return self._kv_store_cmd_socket.recv_thrift_obj(
            kv_store_types.Publication)

    def dump_key_with_prefix(self, prefix=""):
        '''  dump the hashes of kvstore whose key matches the given prefix
             if prefix is an empty string, the full KV hash is dumped
        '''

        req_msg = kv_store_types.Request(kv_store_types.Command.HASH_DUMP)
        req_msg.keyDumpParams = kv_store_types.KeyDumpParams(prefix)
        self._kv_store_cmd_socket.send_thrift_obj(req_msg)

        resp = self._kv_store_cmd_socket.recv()
        if len(resp) == 3 and resp == str('ERR'):
            # KvStore doesn't support HASH_DUMP API yet. Use full dump
            # API instead
            return self.dump_all_with_prefix(prefix)
        else:
            return serializer.deserialize_thrift_object(
                resp,
                kv_store_types.Publication,
                self._kv_store_cmd_socket.proto_factory)

    def dump_peers(self):
        '''  dump the entries of kvstore whose key matches the given prefix
             if prefix is an empty string, the full KV store is dumped
        '''

        req_msg = kv_store_types.Request(kv_store_types.Command.PEER_DUMP)
        self._kv_store_cmd_socket.send_thrift_obj(req_msg)

        return self._kv_store_cmd_socket.recv_thrift_obj(
            kv_store_types.PeerCmdReply)
