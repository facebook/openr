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
from openr.cli.clis import spark
from openr.cli.tests import helpers

from .fixtures import (
    MOCKED_SPARK_NEIGHBORS,
    SPARK_NEIGHBORS_OUTPUT,
    SPARK_NEIGHBORS_OUTPUT_JSON,
)


BASE_MODULE = "openr.cli.clis.spark"
BASE_CMD_MODULE = "openr.cli.commands.spark"


class CliSparkTests(TestCase):
    maxDiff: Optional[int] = None

    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            spark.SparkCli.spark,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    def test_spark_neighbors(self, mocked_openr_client: MagicMock) -> None:
        # Set mock data for testing
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getNeighbors.return_value = MOCKED_SPARK_NEIGHBORS

        # Invoke with no flags & verify output
        invoked_return = self.runner.invoke(
            spark.SparkNeighborCli.neighbors,
            [],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(SPARK_NEIGHBORS_OUTPUT, invoked_return.stdout)

        # Invoke with [--json] & verify output
        invoked_return = self.runner.invoke(
            spark.SparkNeighborCli.neighbors,
            ["--json"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(SPARK_NEIGHBORS_OUTPUT_JSON, invoked_return.stdout)
