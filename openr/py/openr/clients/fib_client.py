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

from openr.Fib import ttypes as fib_types
from openr.utils import socket, consts

import zmq


class FibClient(object):
    def __init__(self, zmq_ctx, fib_cmd_url, timeout=consts.Consts.TIMEOUT_MS,
                 proto_factory=consts.Consts.PROTO_FACTORY):
        self._fib_cmd_socket = socket.Socket(zmq_ctx, zmq.REQ, timeout, proto_factory)
        self._fib_cmd_socket.connect(fib_cmd_url)

    def get_route_db(self):

        req_msg = fib_types.FibRequest(fib_types.FibCommand.ROUTE_DB_GET)
        self._fib_cmd_socket.send_thrift_obj(req_msg)
        return self._fib_cmd_socket.recv_thrift_obj(fib_types.RouteDatabase)
