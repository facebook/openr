#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import, division, print_function, unicode_literals

from builtins import object

import zmq
from openr.clients.openr_client import OpenrClient
from openr.Decision import ttypes as decision_types
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.utils import consts, zmq_socket


class DecisionClient(OpenrClient):
    def __init__(self, cli_opts):
        super(DecisionClient, self).__init__(
            OpenrModuleType.DECISION,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.decision_rep_port),
            cli_opts,
        )

    def _get_db(self, db_type, node_name=""):

        req_msg = decision_types.DecisionRequest()
        req_msg.cmd = db_type
        req_msg.nodeName = node_name

        return self.send_and_recv_thrift_obj(req_msg, decision_types.DecisionReply)

    def get_route_db(self, node_name=""):

        return self._get_db(
            decision_types.DecisionCommand.ROUTE_DB_GET, node_name
        ).routeDb

    def get_adj_dbs(self):

        return self._get_db(decision_types.DecisionCommand.ADJ_DB_GET).adjDbs

    def get_prefix_dbs(self):

        return self._get_db(decision_types.DecisionCommand.PREFIX_DB_GET).prefixDbs
