#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from typing import Optional
from unittest.mock import AsyncMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.py.openr.cli.clis import prefix_mgr
from openr.py.openr.cli.tests import helpers
from openr.thrift.KvStore import thrift_types as openr_kvstore_types

from .fixtures import (
    ADVERTISED_ROUTES_OUTPUT,
    ADVERTISED_ROUTES_OUTPUT_DETAILED,
    ADVERTISED_ROUTES_OUTPUT_JSON,
    MOCKED_ADVERTISED_ROUTES,
    MOCKED_INIT_EVENT_GOOD,
    MOCKED_INIT_EVENT_TIMEOUT,
    MOCKED_INIT_EVENT_WARNING,
)


BASE_MODULE: str = prefix_mgr.__name__
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    @patch(f"{BASE_CMD_MODULE}.PrefixMgrCmd._get_config")
    def test_prefixmgr_advertised_routes(
        self, mocked_openr_config: AsyncMock, mocked_openr_client: AsyncMock
    ) -> None:
        # Set mock data for testing
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_prefixmgr_validate_init_event(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
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
        pass_time = MOCKED_INIT_EVENT_GOOD[
            openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED
        ]
        self.assertEqual("PASS", init_event_pass_state)

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
