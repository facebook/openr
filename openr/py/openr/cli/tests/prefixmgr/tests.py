#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from typing import Optional
from unittest.mock import MagicMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.cli.clis import prefix_mgr
from openr.cli.tests import helpers
from openr.KvStore import ttypes as openr_kvstore_types

from .fixtures import (
    ADVERTISED_ROUTES_OUTPUT,
    ADVERTISED_ROUTES_OUTPUT_DETAILED,
    ADVERTISED_ROUTES_OUTPUT_JSON,
    MOCKED_ADVERTISED_ROUTES,
    MOCKED_INIT_EVENT_GOOD,
    MOCKED_INIT_EVENT_TIMEOUT,
    MOCKED_INIT_EVENT_WARNING,
)


BASE_MODULE = "openr.cli.clis.prefix_mgr"
BASE_CMD_MODULE = "openr.cli.commands.prefix_mgr"


class CliPrefixManagerTests(TestCase):
    maxDiff: Optional[int] = None

    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            prefix_mgr.PrefixMgrCli.prefixmgr,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT_PY)
    @patch(f"{BASE_CMD_MODULE}.PrefixMgrCmd._get_config")
    def test_prefixmgr_advertised_routes(
        self, mocked_openr_config: MagicMock, mocked_openr_client: MagicMock
    ) -> None:
        # Set mock data for testing
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getAdvertisedRoutesFiltered.return_value = (
            MOCKED_ADVERTISED_ROUTES
        )

        tag_map = {
            "NOT_USED_TAG_NAME": {"tagSet": ["not_used_tag"]},
            "TAG_NAME2": {"tagSet": ["65520:822"]},
        }

        mocked_openr_config.return_value = {
            "area_policies": {"definitions": {"openrTag": {"objects": tag_map}}}
        }

        # Invoke with no flags & verify output
        invoked_return = self.runner.invoke(
            prefix_mgr.AdvertisedRoutesCli.show,
            ["--no-detail", "all"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(ADVERTISED_ROUTES_OUTPUT, invoked_return.stdout)

        # Invoke with [--detail] & verify output
        invoked_return = self.runner.invoke(
            prefix_mgr.AdvertisedRoutesCli.show,
            ["--detail", "all"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(ADVERTISED_ROUTES_OUTPUT_DETAILED, invoked_return.stdout)

        # Invoke with [--json] & verify output
        invoked_return = self.runner.invoke(
            prefix_mgr.AdvertisedRoutesCli.show,
            ["--json", "all"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(ADVERTISED_ROUTES_OUTPUT_JSON, invoked_return.stdout)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT_PY)
    def test_prefixmgr_validate_init_event(
        self, mocked_openr_client: MagicMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getInitializationEvents.return_value = (
            MOCKED_INIT_EVENT_GOOD
        )

        invoked_return = self.runner.invoke(
            prefix_mgr.PrefixMgrValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        for i, l in enumerate(stdout_lines):
            print(i, l)
        # The check result is printed on line 0 of stdout in this case
        init_event_pass_state = stdout_lines[0].split(" ")[-1]
        init_event_duration = stdout_lines[1].split(": ")[1]
        pass_time = MOCKED_INIT_EVENT_GOOD[
            openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED
        ]
        self.assertEqual(f"{pass_time}ms", init_event_duration)
        self.assertEqual("PASS", init_event_pass_state)

        # Test pass - duration results in warning
        mocked_returned_connection.getInitializationEvents.return_value = (
            MOCKED_INIT_EVENT_WARNING
        )

        invoked_return = self.runner.invoke(
            prefix_mgr.PrefixMgrValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        init_event_pass_state = stdout_lines[0].split(" ")[-1]
        init_event_duration = stdout_lines[1].split(": ")[1]

        self.assertEqual("PASS", init_event_pass_state)
        pass_time = MOCKED_INIT_EVENT_WARNING[
            openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED
        ]
        self.assertEqual(f"{pass_time}ms", init_event_duration)

        # Test fail - duration results in timeout
        mocked_returned_connection.getInitializationEvents.return_value = (
            MOCKED_INIT_EVENT_TIMEOUT
        )

        invoked_return = self.runner.invoke(
            prefix_mgr.PrefixMgrValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        init_event_pass_state = stdout_lines[0].split(" ")[-1]
        err_msg = stdout_lines[1]
        init_event_duration = stdout_lines[2].split(": ")[1]

        self.assertEqual("FAIL", init_event_pass_state)
        self.assertEqual(
            "PREFIX_DB_SYNCED event duration exceeds acceptable time limit (>300000ms)",
            err_msg,
        )
        pass_time = MOCKED_INIT_EVENT_TIMEOUT[
            openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED
        ]
        self.assertEqual(f"{pass_time}ms", init_event_duration)

        # Test fail - PREFIX_DB_SYNCED is not published
        mocked_returned_connection.getInitializationEvents.return_value = {}

        invoked_return = self.runner.invoke(
            prefix_mgr.PrefixMgrValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        init_event_pass_state = stdout_lines[0].split(" ")[-1]
        err_msg = stdout_lines[1]

        self.assertEqual("FAIL", init_event_pass_state)
        self.assertEqual("PREFIX_DB_SYNCED event is not published", err_msg)
