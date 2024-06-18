#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

import asyncio
from typing import Dict, List, Optional, Sequence
from unittest.mock import AsyncMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.py.openr.cli.clis import fib
from openr.py.openr.cli.tests import helpers
from openr.py.openr.cli.utils import utils
from openr.py.openr.utils import ipnetwork
from openr.thrift.Network.thrift_types import NextHopThrift, UnicastRoute

from .fixtures import (
    MOCKED_ADJDB,
    MOCKED_UNICAST_ROUTELIST,
    MOCKED_UNICAST_ROUTELIST_MULTIPLE,
)


class CliFibTests(TestCase):
    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            fib.FibCli.fib,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    def check_address(self, addr_line: str, expected_addr: str) -> None:
        """
        Checks if the expected address is found in addr_line
        """

        tokenized_addr_line = addr_line.split()

        self.assertTrue(
            expected_addr in tokenized_addr_line,
            "Address is incorrect or is missing"
            + f"\n Checked Line (Tokenized): {tokenized_addr_line}, Expected Address: {expected_addr}",
        )

    def check_nexthop(
        self,
        nexthop_line: str,
        expected_nexthop: NextHopThrift,
        addr_to_name: Optional[Dict[bytes, str]] = None,
    ) -> None:
        """
        Checks  if the next hop information outputted is correct
        If all lines pass then the check passes
        """

        nexthop_addr = expected_nexthop.address
        nexthop_name = (
            addr_to_name[nexthop_addr.addr]
            if addr_to_name
            else ipnetwork.sprint_addr(nexthop_addr.addr)
        )
        ifname = (
            f"%{nexthop_addr.ifName}" if (nexthop_addr.ifName or addr_to_name) else ""
        )
        expected_nexthop_str = f"{nexthop_name}{ifname}"

        self.check_address(nexthop_line, expected_nexthop_str)

    def check_printed_routes(
        self,
        stdout_lines: List[str],
        expected_routes: List[UnicastRoute],
        addr_to_name: Optional[Dict[bytes, str]] = None,
    ) -> None:
        """
        Checks if each address in the output is correct
        """
        unicast_route_dict: Dict[str, Sequence[NextHopThrift]] = {
            ipnetwork.sprint_prefix(route.dest): route.nextHops
            for route in expected_routes
        }

        def check_dest_line(
            dest_line: str,
        ) -> Optional[Sequence[NextHopThrift]]:

            is_valid_route = False
            next_hops = None

            tokenized_line = dest_line.split()
            for token in tokenized_line:
                if (
                    ipnetwork.is_ip_addr(token, strict=False)
                    and token in unicast_route_dict
                ):
                    is_valid_route = True
                    next_hops = unicast_route_dict[token]
                    del unicast_route_dict[token]  # Will fail duplicates
                    break

            self.assertTrue(
                is_valid_route,
                "No Valid Destination Address"
                + f"\n Checked Line (Tokenized): {tokenized_line}",
            )

            return next_hops

        line_idx = 0
        while line_idx < len(stdout_lines):
            if stdout_lines[line_idx][0] == ">":
                # A new route, check dest and next hops

                next_hops = check_dest_line(stdout_lines[line_idx])
                if next_hops is None:
                    return  # this would have already failed a check

                for nh_idx, nh in enumerate(next_hops, 1):
                    nh_line = stdout_lines[line_idx + nh_idx]
                    self.check_nexthop(nh_line, nh, addr_to_name)

                line_idx += len(next_hops)
            else:
                line_idx += 1

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_fib_unicast_routes_simple(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )

        mocked_returned_connection.getUnicastRoutesFiltered.return_value = (
            MOCKED_UNICAST_ROUTELIST
        )

        # Basic check, all default options, only one unicast route
        invoked_return = self.runner.invoke(
            fib.FibUnicastRoutesCli.routes,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        stdout_lines = [line for line in stdout_lines if line != ""]

        self.check_printed_routes(stdout_lines, MOCKED_UNICAST_ROUTELIST)

        # Checking with hostnames option
        # Fib builds the map from address to nodename using the adjacency db
        mocked_returned_connection.getDecisionAdjacenciesFiltered.return_value = [
            MOCKED_ADJDB
        ]
        invoked_return = self.runner.invoke(
            fib.FibUnicastRoutesCli.routes,
            ["--hostnames"],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        stdout_lines = [line for line in stdout_lines if line != ""]

        # Get address to node name map
        addr_to_name = asyncio.run(
            utils.adjs_nexthop_to_neighbor_name(mocked_returned_connection)
        )
        self.check_printed_routes(stdout_lines, MOCKED_UNICAST_ROUTELIST, addr_to_name)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_fib_unicast_routes_multiple_routes(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        # Checks with multiple routes
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )

        mocked_returned_connection.getUnicastRoutesFiltered.return_value = (
            MOCKED_UNICAST_ROUTELIST_MULTIPLE
        )

        invoked_return = self.runner.invoke(
            fib.FibUnicastRoutesCli.routes,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        stdout_lines = [line for line in stdout_lines if line != ""]

        self.check_printed_routes(stdout_lines, MOCKED_UNICAST_ROUTELIST_MULTIPLE)

        mocked_returned_connection.getDecisionAdjacenciesFiltered.return_value = [
            MOCKED_ADJDB
        ]
        invoked_return = self.runner.invoke(
            fib.FibUnicastRoutesCli.routes,
            ["--hostnames"],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        stdout_lines = [line for line in stdout_lines if line != ""]

        addr_to_name = asyncio.run(
            utils.adjs_nexthop_to_neighbor_name(mocked_returned_connection)
        )
        self.check_printed_routes(
            stdout_lines, MOCKED_UNICAST_ROUTELIST_MULTIPLE, addr_to_name
        )
