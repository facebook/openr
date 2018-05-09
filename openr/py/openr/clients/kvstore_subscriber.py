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

from openr.KvStore import ttypes as kv_store_types
from openr.utils import socket, consts

import zmq


class KvStoreSubscriber(object):
    def __init__(self, zmq_ctx, kv_store_pub_url, timeout=-1,
                 proto_factory=consts.Consts.PROTO_FACTORY):

        # timeout set as -1 for indefinite blocking
        self._kv_store_sub_socket = socket.Socket(zmq_ctx, zmq.SUB, timeout,
                                                  proto_factory)
        self._kv_store_sub_socket.connect(kv_store_pub_url)
        self._kv_store_sub_socket.set_sock_opt(zmq.SUBSCRIBE, b"")

    def listen(self):
        return self._kv_store_sub_socket.recv_thrift_obj(
            kv_store_types.Publication)
