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
from openr.cli.clis import decision
from openr.py.openr.cli.tests import helpers

from .fixtures import (
    AREA_SUMMARIES,
    DECISION_ADJ_DBS_OK,
    EXPECTED_ROUTES_RECEIVED_JSON,
    EXPECTED_VALIDATE_OUTPUT_NO_PUBLISH,
    EXPECTED_VALIDATE_OUTPUT_OK,
    KVSTORE_KEYVALS_OK,
    MOCKED_INIT_EVENTS_PASS,
    MOCKED_RECEIVED_ROUTES,
    RECEIVED_ROUTES_DB_OK,
)


BASE_MODULE = "openr.cli.clis.decision"
BASE_CMD_MODULE = "openr.cli.commands.decision"


class CliDecisionTests(TestCase):
    maxDiff: Optional[int] = None

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
