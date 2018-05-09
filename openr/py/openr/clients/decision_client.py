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

from openr.Decision import ttypes as decision_types
from openr.utils import socket, consts

import zmq


class DecisionClient(object):
    def __init__(self, zmq_ctx, decision_cmd_url, timeout=consts.Consts.TIMEOUT_MS,
                 proto_factory=consts.Consts.PROTO_FACTORY):
        self._decision_cmd_socket = socket.Socket(zmq_ctx, zmq.REQ, timeout,
                                                  proto_factory)
        self._decision_cmd_socket.connect(decision_cmd_url)

    def _get_db(self, db_type, node_name=''):

        req_msg = decision_types.DecisionRequest()
        req_msg.cmd = db_type
        req_msg.nodeName = node_name

        self._decision_cmd_socket.send_thrift_obj(req_msg)
        return self._decision_cmd_socket.recv_thrift_obj(decision_types.DecisionReply)

    def get_route_db(self, node_name=''):

        return self._get_db(decision_types.DecisionCommand.ROUTE_DB_GET,
                            node_name).routeDb

    def get_adj_dbs(self):

        return self._get_db(decision_types.DecisionCommand.ADJ_DB_GET).adjDbs

    def get_prefix_dbs(self):

        return self._get_db(decision_types.DecisionCommand.PREFIX_DB_GET).prefixDbs
