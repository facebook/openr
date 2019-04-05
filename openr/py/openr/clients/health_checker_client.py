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
from openr.HealthChecker import ttypes as health_checker_types
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.utils import consts, zmq_socket


class HealthCheckerClient(OpenrClient):
    def __init__(self, cli_opts):
        super(HealthCheckerClient, self).__init__(
            OpenrModuleType.HEALTH_CHECKER,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.health_checker_cmd_port),
            cli_opts,
        )

    def peek(self):
        req_msg = health_checker_types.HealthCheckerRequest()
        req_msg.cmd = health_checker_types.HealthCheckerCmd.PEEK

        return self.send_and_recv_thrift_obj(
            req_msg, health_checker_types.HealthCheckerPeekReply
        )
