#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


from builtins import object

import zmq
from openr.clients.openr_client import OpenrClientDeprecated
from openr.Fib import ttypes as fib_types
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.utils import consts, zmq_socket


class FibClient(OpenrClientDeprecated):
    def __init__(self, cli_opts):
        super(FibClient, self).__init__(
            OpenrModuleType.FIB,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.fib_rep_port),
            cli_opts,
        )

    def get_route_db(self):
        req_msg = fib_types.FibRequest(fib_types.FibCommand.ROUTE_DB_GET)
        return self.send_and_recv_thrift_obj(req_msg, fib_types.RouteDatabase)
