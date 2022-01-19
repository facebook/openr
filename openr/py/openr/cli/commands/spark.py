#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from typing import List

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.OpenrCtrl import OpenrCtrl
from openr.Types import ttypes as openr_types
from openr.utils import printing, serializer


class NeighborCmd(OpenrCtrlCmd):
    def _run(
        self, client: OpenrCtrl.Client, json: bool, detailed: bool, *args, **kwargs
    ) -> None:

        # Get data
        neighbors = self.fetch(client)

        # Render
        if json:
            print(serializer.serialize_json(neighbors))
        else:
            self.render(neighbors, detailed)

    def fetch(self, client: OpenrCtrl.Client) -> List[openr_types.SparkNeighbor]:
        """
        Fetch the requested data
        """

        return client.getNeighbors()

    def render(
        self, neighbors: List[openr_types.SparkNeighbor], detailed: bool
    ) -> None:
        """
        Render neighbors with or without details
        """

        # print out neighbors horizontally
        rows = []
        column_labels = [
            "Neighbor",
            "State",
            "Latest Event",
            "Local Intf",
            "Remote Intf",
            "Area",
            "Rtt(us)",
        ]
        for neighbor in sorted(neighbors, key=lambda neighbor: neighbor.nodeName):
            rows.append(
                [
                    neighbor.nodeName,
                    neighbor.state,
                    neighbor.event,
                    neighbor.localIfName,
                    neighbor.remoteIfName,
                    neighbor.area,
                    neighbor.rttUs,
                ]
            )
        print("\n", printing.render_horizontal_table(rows, column_labels))


class GracefulRestartCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        yes: bool = False,
        *args,
        **kwargs,
    ) -> None:
        question_str = "Are you sure to force sending GR msg to neighbors?"
        if not utils.yesno(question_str, yes):
            print()
            return

        client.floodRestartingMsg()
        print("Successfully forcing to send GR msgs.\n")
