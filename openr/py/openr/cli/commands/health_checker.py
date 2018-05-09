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

from openr.clients import health_checker_client
from openr.utils import ipnetwork

import tabulate


class HealthCheckerCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Health Checker client '''

        self.client = health_checker_client.HealthCheckerClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.health_checker_cmd_port),
            cli_opts.timeout,
            cli_opts.proto_factory)


class PeekCmd(HealthCheckerCmd):
    def run(self):
        resp = self.client.peek()
        headers = ['Node', 'IP Address', 'Last Value Sent',
                   'Last Ack From Node', 'Last Ack To Node']
        rows = []
        for name, node in resp.nodeInfo.items():
            rows.append([
                name,
                ipnetwork.sprint_addr(node.ipAddress.addr),
                node.lastValSent,
                node.lastAckFromNode,
                node.lastAckToNode
            ])

        print()
        print(tabulate.tabulate(rows, headers=headers))
        print()
