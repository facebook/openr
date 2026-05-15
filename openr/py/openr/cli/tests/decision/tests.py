#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from copy import deepcopy
from typing import Optional
from unittest.mock import AsyncMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.py.openr.cli.clis import decision
from openr.py.openr.cli.commands.decision import PathCmd
from openr.py.openr.cli.tests import helpers

from .fixtures import (
    AREA_SUMMARIES,
    DECISION_ADJ_DBS_MULTI_AREA,
    DECISION_ADJ_DBS_OK,
    EMPTY_KVSTORE_PUBLICATION,
    EMPTY_ROUTE_DB,
    EXPECTED_ROUTES_RECEIVED_JSON,
    EXPECTED_VALIDATE_OUTPUT_NO_PUBLISH,
    EXPECTED_VALIDATE_OUTPUT_OK,
    KVSTORE_KEYVALS_OK,
    MOCKED_INIT_EVENTS_PASS,
    MOCKED_RECEIVED_ROUTES,
    RECEIVED_ROUTES_DB_OK,
)


BASE_MODULE: str = decision.__name__
BASE_CMD_MODULE = "openr.py.openr.cli.commands.decision"


class CliDecisionTests(TestCase):
    maxDiff: int | None = None

    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            decision.DecisionCli.decision,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_decision_validate_all_areas(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        # Have some Areas Returned
        mocked_returned_connection.getKvStoreAreaSummary.return_value = AREA_SUMMARIES
        # Have decision adjacencies returned
        mocked_returned_connection.getDecisionAdjacenciesFiltered.return_value = (
            DECISION_ADJ_DBS_OK
        )
        # Have routes returned
        mocked_returned_connection.getReceivedRoutesFiltered.return_value = (
            RECEIVED_ROUTES_DB_OK
        )
        # Have kvstore data returned
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.return_value = (
            KVSTORE_KEYVALS_OK
        )

        # Have published initialization events returned
        mocked_returned_connection.getInitializationEvents.return_value = (
            MOCKED_INIT_EVENTS_PASS
        )

        with patch("openr.py.openr.cli.utils.utils.get_area_id", return_value=69):
            invoked_return = self.runner.invoke(
                decision.DecisionValidateCli.validate,
                [],  # No args
                catch_exceptions=False,
            )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(EXPECTED_VALIDATE_OUTPUT_OK, invoked_return.stdout)

        # Test bad - initialization event not published
        mocked_returned_connection.getInitializationEvents.return_value = {}

        with patch("openr.py.openr.cli.utils.utils.get_area_id", return_value=69):
            invoked_return = self.runner.invoke(
                decision.DecisionValidateCli.validate,
                [],  # No args
                catch_exceptions=False,
            )
        self.assertEqual(1, invoked_return.exit_code)
        self.assertEqual(EXPECTED_VALIDATE_OUTPUT_NO_PUBLISH, invoked_return.stdout)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_decision_received_routes_json(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        # Retturn a List of ReceivedRouteDetail
        mocked_returned_connection.getReceivedRoutesFiltered.return_value = (
            MOCKED_RECEIVED_ROUTES
        )
        invoked_return = self.runner.invoke(
            decision.ReceivedRoutesCli.show,
            ["--json"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(EXPECTED_ROUTES_RECEIVED_JSON, invoked_return.stdout)

    def test_path_get_if2node_map_merges_multi_area(self) -> None:
        """On a multi-area (ABR) node, the same thisNodeName appears once per
        area. get_if2node_map must merge all per-area adjacencies into a single
        per-node nexthop dict instead of overwriting (the old iter_dbs path
        filtered to a single area)."""
        if2node = PathCmd().get_if2node_map(DECISION_ADJ_DBS_MULTI_AREA)

        self.assertIn("abr-node", if2node)
        # Both per-area adjacencies for "abr-node" must survive the merge.
        nexthop_dict = if2node["abr-node"]
        self.assertEqual(
            "peer-area1",
            nexthop_dict[("if-area1", "fe80::1")],
        )
        self.assertEqual(
            "peer-area2",
            nexthop_dict[("if-area2", "fe80::2")],
        )

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_decision_path_with_area(self, mocked_openr_client: AsyncMock) -> None:
        """`breeze decision path --area <area> ...` must use the area-aware
        adjacency API restricted to the given area (T267753415)."""
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getDecisionAreaAdjacenciesFiltered.return_value = {}
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.return_value = (
            EMPTY_KVSTORE_PUBLICATION
        )
        mocked_returned_connection.getRouteDbComputed.return_value = EMPTY_ROUTE_DB

        with patch(
            "openr.py.openr.cli.utils.utils.get_area_id",
            new=AsyncMock(return_value="rb_rb_plane002"),
        ):
            invoked_return = self.runner.invoke(
                decision.PathCli.path,
                [
                    "--area",
                    "rb_rb_plane002",
                    "--src",
                    "src-node",
                    "--dst",
                    "fdad:ff01:31e::d:0/128",
                ],
                catch_exceptions=False,
            )

        self.assertEqual(0, invoked_return.exit_code)

        # The fix replaces getDecisionAdjacencyDbs() (single-area, throws on
        # multi-area configs) and the older getDecisionAdjacenciesFiltered()
        # with the area-keyed getDecisionAreaAdjacenciesFiltered.
        mocked_returned_connection.getDecisionAdjacencyDbs.assert_not_awaited()
        mocked_returned_connection.getDecisionAdjacenciesFiltered.assert_not_awaited()
        mocked_returned_connection.getDecisionAreaAdjacenciesFiltered.assert_awaited_once()
        (filter_arg,) = (
            mocked_returned_connection.getDecisionAreaAdjacenciesFiltered.await_args.args
        )
        self.assertEqual({"rb_rb_plane002"}, set(filter_arg.selectAreas))

        # KvStore prefix dump is restricted to the requested area.
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.assert_awaited_once()
        kv_call = mocked_returned_connection.getKvStoreKeyValsFilteredArea.await_args
        self.assertEqual("rb_rb_plane002", kv_call.args[1])

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_decision_path_without_area(self, mocked_openr_client: AsyncMock) -> None:
        """`--area` is optional; when omitted, `get_area_id` resolves the
        default (the single configured area on a single-area node, or an error
        on multi-area nodes). The thrift call must still go through the
        area-aware adjacency API with the resolved area."""
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getDecisionAreaAdjacenciesFiltered.return_value = {}
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.return_value = (
            EMPTY_KVSTORE_PUBLICATION
        )
        mocked_returned_connection.getRouteDbComputed.return_value = EMPTY_ROUTE_DB

        with patch(
            "openr.py.openr.cli.utils.utils.get_area_id",
            new=AsyncMock(return_value="default-area"),
        ):
            invoked_return = self.runner.invoke(
                decision.PathCli.path,
                ["--src", "src-node", "--dst", "fdad:ff01:31e::d:0/128"],
                catch_exceptions=False,
            )

        self.assertEqual(0, invoked_return.exit_code)
        mocked_returned_connection.getDecisionAreaAdjacenciesFiltered.assert_awaited_once()
        (filter_arg,) = (
            mocked_returned_connection.getDecisionAreaAdjacenciesFiltered.await_args.args
        )
        self.assertEqual({"default-area"}, set(filter_arg.selectAreas))

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_decision_received_routes_json_no_data(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        # Retturn a List of ReceivedRouteDetail
        mocked_returned_connection.getReceivedRoutesFiltered.return_value = []
        invoked_return = self.runner.invoke(
            decision.ReceivedRoutesCli.show,
            ["--json"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)  # TODO - Should we return 1?
        self.assertEqual("[]\n", invoked_return.stdout)
