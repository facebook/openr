#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from typing import List, Optional
from unittest.mock import MagicMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.cli.clis import spark
from openr.cli.tests import helpers

from .fixtures import (
    MOCKED_SPARK_NEIGHBORS,
    MOCKED_SPARK_NEIGHBORS_ALL_ESTAB,
    MOCKED_SPARK_NEIGHBORS_NO_ESTAB,
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

    def _parse_validate_stdout(self, validate_stdout: str) -> List[str]:
        """
        Checks if stdout is missing information, returns stdout as a list of strings
        """

        stdout_lines = validate_stdout.split("\n")
        neighbor_stat_line = ""
        neighbor_pass_state_line = ""
        for line in stdout_lines:
            if "Neighbor State Check" in line:
                neighbor_pass_state_line = line
            if "Total Neighbors" in line:
                neighbor_stat_line = line
                break
        self.assertNotEqual(
            "", neighbor_stat_line, "Validation output missing neighbor stat"
        )
        self.assertNotEqual(
            "",
            neighbor_pass_state_line,
            "Validation output missing neighbor pass state",
        )
        return stdout_lines

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    def test_spark_validate(self, mocked_openr_client: MagicMock) -> None:
        # Since print output uses neighbor command, we just validate if numbers are correct
        # Testing with one non ESTABLISHED neighbor node and one ESTABLISHED node
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getNeighbors.return_value = MOCKED_SPARK_NEIGHBORS

        # Validate with no flags set
        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        # Get neighbor info from parsing stdout
        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        neighbor_pass_state_line = stdout_lines[0]
        neighbor_stat_line = stdout_lines[1]
        tokenized_neighbor_stat = neighbor_stat_line.split(": ")
        neighbor_pass_state = neighbor_pass_state_line.split(" ")[-1]

        self.assertEqual("FAIL", neighbor_pass_state)
        self.assertEqual(
            "2", tokenized_neighbor_stat[1][0], "Incorrect total number of neighbors"
        )
        self.assertEqual(
            "1",
            tokenized_neighbor_stat[2][0],
            "Incorrect number of ESTABLISHED neighbors",
        )
        self.assertEqual(
            "1",
            tokenized_neighbor_stat[3][0],
            "Incorrect number of non-ESTABLISHED neighbors",
        )

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    def test_spark_validate_all_estab(self, mocked_openr_client: MagicMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getNeighbors.return_value = (
            MOCKED_SPARK_NEIGHBORS_ALL_ESTAB
        )

        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        tokenized_neighbor_stat = stdout_lines[1].split(": ")
        neighbor_pass_state = stdout_lines[0].split(" ")[-1]

        self.assertEqual("PASS", neighbor_pass_state)
        self.assertEqual(
            "2", tokenized_neighbor_stat[1][0], "Incorrect total number of neighbors"
        )
        self.assertEqual(
            "2",
            tokenized_neighbor_stat[2][0],
            "Incorrect number of ESTABLISHED neighbors",
        )
        self.assertEqual(
            "0",
            tokenized_neighbor_stat[3][0],
            "Incorrect number of non-ESTABLISHED neighbors",
        )

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    def test_spark_validate_no_estab(self, mocked_openr_client: MagicMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getNeighbors.return_value = (
            MOCKED_SPARK_NEIGHBORS_NO_ESTAB
        )

        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        tokenized_neighbor_stat = stdout_lines[1].split(": ")
        neighbor_pass_state = stdout_lines[0].split(" ")[-1]

        self.assertEqual("FAIL", neighbor_pass_state)
        self.assertEqual(
            "2", tokenized_neighbor_stat[1][0], "Incorrect total number of neighbors"
        )
        self.assertEqual(
            "0",
            tokenized_neighbor_stat[2][0],
            "Incorrect number of ESTABLISHED neighbors",
        )
        self.assertEqual(
            "2",
            tokenized_neighbor_stat[3][0],
            "Incorrect number of non-ESTABLISHED neighbors",
        )

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    def test_spark_validate_no_neighbors(self, mocked_openr_client: MagicMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getNeighbors.return_value = []

        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        tokenized_neighbor_stat = stdout_lines[1].split(": ")
        neighbor_pass_state = stdout_lines[0].split(" ")[-1]

        self.assertEqual("PASS", neighbor_pass_state)
        self.assertEqual(
            "0", tokenized_neighbor_stat[1][0], "Incorrect total number of neighbors"
        )
        self.assertEqual(
            "0",
            tokenized_neighbor_stat[2][0],
            "Incorrect number of ESTABLISHED neighbors",
        )
        self.assertEqual(
            "0",
            tokenized_neighbor_stat[3][0],
            "Incorrect number of non-ESTABLISHED neighbors",
        )
