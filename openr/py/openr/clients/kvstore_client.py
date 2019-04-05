#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


from builtins import object, str
from typing import Dict, List, Optional

import zmq
from openr.clients.openr_client import OpenrClient
from openr.KvStore import ttypes as kv_store_types
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.utils import consts, serializer, zmq_socket


class KvStoreClient(OpenrClient):
    def __init__(self, cli_opts, host=None):
        host = host if host else cli_opts.host
        zmq_endpoint = "tcp://[{}]:{}".format(host, cli_opts.kv_rep_port)
        super(KvStoreClient, self).__init__(
            OpenrModuleType.KVSTORE, zmq_endpoint, cli_opts
        )

    def get_keys(self, keys):
        """ Get values corresponding to keys from KvStore.
            It gets from local snapshot KeyVals of the kvstore.
        """

        req_msg = kv_store_types.Request(kv_store_types.Command.KEY_GET)
        req_msg.keyGetParams = kv_store_types.KeyGetParams(keys)

        return self.send_and_recv_thrift_obj(req_msg, kv_store_types.Publication)

    def set_key(self, keyVals):

        req_msg = kv_store_types.Request(kv_store_types.Command.KEY_SET)
        req_msg.keySetParams = kv_store_types.KeySetParams(keyVals)

        return self.send_and_recv_thrift_obj(req_msg, str)

    def dump_all_with_filter(
        self,
        prefix: str = "",
        originator_ids: Optional[List[str]] = None,
        keyval_hash: Optional[Dict[str, kv_store_types.Value]] = None,
    ):
        """  dump the entries of kvstore whose key matches the given prefix
             if prefix is an empty string, the full KV store is dumped
        """

        req_msg = kv_store_types.Request(kv_store_types.Command.KEY_DUMP)
        req_msg.keyDumpParams = kv_store_types.KeyDumpParams(prefix)
        req_msg.keyDumpParams.originatorIds = []
        req_msg.keyDumpParams.keyValHashes = None
        if originator_ids:
            req_msg.keyDumpParams.originatorIds = originator_ids
        if keyval_hash:
            req_msg.keyDumpParams.keyValHashes = keyval_hash

        return self.send_and_recv_thrift_obj(req_msg, kv_store_types.Publication)

    def dump_key_with_prefix(self, prefix=""):
        """  dump the hashes of kvstore whose key matches the given prefix
             if prefix is an empty string, the full KV hash is dumped
        """

        req_msg = kv_store_types.Request(kv_store_types.Command.HASH_DUMP)
        req_msg.keyDumpParams = kv_store_types.KeyDumpParams(prefix)

        return self.send_and_recv_thrift_obj(req_msg, kv_store_types.Publication)

    def dump_peers(self):
        """  dump the entries of kvstore whose key matches the given prefix
             if prefix is an empty string, the full KV store is dumped
        """

        req_msg = kv_store_types.Request(kv_store_types.Command.PEER_DUMP)
        return self.send_and_recv_thrift_obj(req_msg, kv_store_types.PeerCmdReply)

    def get_spt_infos(self):
        """ get all spanning tree infos
        """

        req_msg = kv_store_types.Request(kv_store_types.Command.FLOOD_TOPO_GET)
        return self.send_and_recv_thrift_obj(req_msg, kv_store_types.SptInfos)
