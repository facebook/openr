#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import tabulate
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import ipnetwork


class PeekCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client) -> None:
        resp = client.getHealthCheckerInfo()
        headers = [
            "Node",
            "IP Address",
            "Last Value Sent",
            "Last Ack From Node",
            "Last Ack To Node",
        ]
        rows = []
        for name, node in resp.nodeInfo.items():
            rows.append(
                [
                    name,
                    ipnetwork.sprint_addr(node.ipAddress.addr),
                    node.lastValSent,
                    node.lastAckFromNode,
                    node.lastAckToNode,
                ]
            )

        print()
        print(tabulate.tabulate(rows, headers=headers))
        print()
