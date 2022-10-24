#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import datetime
from typing import Any, Dict, List, Tuple

import click

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmdPy
from openr.KvStore import ttypes as kv_store_types
from openr.OpenrCtrl import OpenrCtrl
from openr.thrift.KvStore.thrift_types import InitializationEvent
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing, serializer


class SparkBaseCmd(OpenrCtrlCmdPy):
    def print_spark_neighbors_detailed(
        self, neighbors: List[openr_types.SparkNeighbor]
    ) -> None:
        """
        Construct print lines of Spark neighbors in detailed fashion
        """

        rows = []

        for neighbor in neighbors:
            v4Addr = (ipnetwork.sprint_addr(neighbor.transportAddressV4.addr),)
            v6Addr = (ipnetwork.sprint_addr(neighbor.transportAddressV6.addr),)
            helloMsgSentTimeDelta = str(
                datetime.timedelta(milliseconds=neighbor.lastHelloMsgSentTimeDelta)
            )
            handshakeMsgSentTimeDelta = str(
                datetime.timedelta(milliseconds=neighbor.lastHandshakeMsgSentTimeDelta)
            )
            heartbeatMsgSentTimeDelta = str(
                datetime.timedelta(milliseconds=neighbor.lastHeartbeatMsgSentTimeDelta)
            )

            # Top tier information for neighbor
            rows.append("")
            rows.append(
                f"Neighbor: {neighbor.nodeName}, "
                f"State: {neighbor.state}, "
                f"Last Event: {neighbor.event}"
            )
            # Neighbor attributes
            rows.append("\t[Transport Attributes]:")
            rows.append(
                f"\t\tNeighbor V4 Addr: {v4Addr}\n"
                f"\t\tNeighbor V6 Addr: {v6Addr}\n"
                f"\t\tLocal Interface: {neighbor.localIfName}\n"
                f"\t\tRemote Interface: {neighbor.remoteIfName}\n"
            )
            rows.append("\t[Other Attributes]:")
            rows.append(
                f"\t\tAreaId: {neighbor.area}\n"
                f"\t\tRtt(us): {neighbor.rttUs}\n"
                f"\t\tTCP port: {neighbor.openrCtrlThriftPort}\n"
            )
            # Spark ctrl msg info
            rows.append(f"Last SparkHelloMsg sent: {helloMsgSentTimeDelta} ago")
            rows.append(f"Last SparkHandshakeMsg sent: {handshakeMsgSentTimeDelta} ago")
            rows.append(f"Last SparkHeartbeatMsg sent: {heartbeatMsgSentTimeDelta} ago")

        print("\n".join(rows))

    def print_spark_neighbors(self, neighbors: List[openr_types.SparkNeighbor]) -> None:
        """
        Render neighbors without details
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

    def fetch_spark_neighbors(
        self, client: OpenrCtrl.Client
    ) -> List[openr_types.SparkNeighbor]:
        """
        Fetch the Spark neighbors thrift structure via thrift call
        """

        return client.getNeighbors()


class NeighborCmd(SparkBaseCmd):
    def _run(
        self, client: OpenrCtrl.Client, json: bool, detailed: bool, *args, **kwargs
    ) -> None:

        # Get data
        neighbors = self.fetch_spark_neighbors(client)

        # Render
        if json:
            print(serializer.serialize_json(neighbors))
        else:
            self.render(neighbors, detailed)

    def render(
        self, neighbors: List[openr_types.SparkNeighbor], detailed: bool
    ) -> None:
        """
        Render the received Spark neighbor data
        """

        if detailed:
            self.print_spark_neighbors_detailed(neighbors)
        else:
            self.print_spark_neighbors(neighbors)


class ValidateCmd(SparkBaseCmd):
    def _run(self, client: OpenrCtrl.Client, detail: bool, *args, **kwards) -> bool:

        is_pass = True

        # Get data
        neighbors = self.fetch_spark_neighbors(client)
        initialization_events = self.fetch_initialization_events_py(client)
        openr_config = self.fetch_running_config_thrift(client)

        # Validate spark details
        state_non_estab_neighbors = self._validate_neighbor_state(neighbors)

        is_pass = is_pass and (len(state_non_estab_neighbors) == 0)

        init_is_pass, init_err_msg_str, init_dur_str = self.validate_init_event_py(
            initialization_events,
            kv_store_types.InitializationEvent.NEIGHBOR_DISCOVERED,
        )

        is_pass = is_pass and init_is_pass

        (
            regex_invalid_neighbors,
            regex_dict,
        ) = self._validate_neigbor_regex(neighbors, openr_config.areas)

        is_pass = is_pass and (len(regex_invalid_neighbors) == 0)

        # Render
        self._print_neighbor_info(state_non_estab_neighbors, len(neighbors), detail)
        self.print_initialization_event_check(
            init_is_pass,
            init_err_msg_str,
            init_dur_str,
            InitializationEvent.NEIGHBOR_DISCOVERED,
            "spark",
        )
        self._print_neighbor_regex_info(regex_invalid_neighbors, regex_dict, detail)

        return is_pass

    def _validate_neighbor_state(
        self, neighbors: List[openr_types.SparkNeighbor]
    ) -> List[openr_types.SparkNeighbor]:
        """
        Returns a list of neighbors not in ESTABLISHED state.
        If there are none, the list returned is empty
        """

        # Getting numeric neighbor info
        non_estab_neighbors = [
            neighbor for neighbor in neighbors if neighbor.state != "ESTABLISHED"
        ]

        return non_estab_neighbors

    def _validate_neigbor_regex(
        self, neighbors: List[openr_types.SparkNeighbor], areas: List[Any]
    ) -> Tuple[List[openr_types.SparkNeighbor], Dict[str, List[str]]]:
        """
        Returns a list of all neighbors which don't pass the check
        and a dictionary of area_id : neighbor_regexes
        """

        invalid_neighbors = set()

        area_neighbor_regex_dict = {}
        for area in areas:
            area_neighbor_regex_dict[area.area_id] = area.neighbor_regexes

        for neighbor in neighbors:
            is_valid_neighbor = self.validate_regexes(
                area_neighbor_regex_dict[neighbor.area],
                [neighbor.nodeName],
                True,  # Expect atleat one regex match
            )

            if not is_valid_neighbor:
                invalid_neighbors.add(neighbor)

        return (
            list(invalid_neighbors),
            area_neighbor_regex_dict,
        )

    def _print_neighbor_info(
        self,
        non_estab_neighbors: List[openr_types.SparkNeighbor],
        total_neighbors: int,
        detail: bool,
    ) -> None:
        """
        Print how many neighbors have ESTABLISHED state vs non ESTABLISHED state.
        Print information about non ESTABLISHED neighbors
        """

        num_non_estab = len(non_estab_neighbors)
        num_estab = total_neighbors - num_non_estab

        click.echo(
            self.validation_result_str(
                "spark", "neighbor state check", (num_non_estab == 0)
            )
        )

        # Print Neigbor stats
        estab_str = click.style(f"{num_estab}", fg="green")
        non_estab_str = click.style(f"{num_non_estab}", fg="red")
        click.echo(
            f"Total Neighbors: {total_neighbors}, Neigbors in ESTABLISHED State: {estab_str}, Neighbors in Other States: {non_estab_str}"
        )

        # Print Neigbor info in horizontal table
        if not (num_non_estab == 0):
            click.echo("[Spark] Information about Neighbors in Other States")
            if detail:
                self.print_spark_neighbors_detailed(non_estab_neighbors)
            else:
                self.print_spark_neighbors(non_estab_neighbors)

    def _print_neighbor_regex_info(
        self,
        invalid_neighbors: List[openr_types.SparkNeighbor],
        regexes: Dict[str, List[str]],
        detail: bool,
    ) -> None:

        click.echo(
            self.validation_result_str(
                "spark", "neighbor regex matching check", (len(invalid_neighbors) == 0)
            )
        )
        click.echo(f"Neighbor Regexes: {regexes}")

        if not (len(invalid_neighbors) == 0):
            click.echo(
                "[Spark] Information about neighbors not matching any neighbor regexes"
            )
            if detail:
                self.print_spark_neighbors_detailed(invalid_neighbors)
            else:
                self.print_spark_neighbors(invalid_neighbors)


class GracefulRestartCmd(OpenrCtrlCmdPy):
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
