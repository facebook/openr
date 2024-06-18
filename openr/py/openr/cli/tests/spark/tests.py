#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from typing import List, Optional
from unittest.mock import AsyncMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.py.openr.cli.clis import spark
from openr.py.openr.cli.tests import helpers

from .fixtures import (
    MOCKED_CONFIG_ACCEPT_ALIENS_NODES,
    MOCKED_CONFIG_ACCEPT_ALL,
    MOCKED_CONFIG_ACCEPT_NONE,
    MOCKED_CONFIG_DEFAULT,
    MOCKED_INIT_EVENTS,
    MOCKED_INIT_EVEVENTS_NO_PUBLISH,
    MOCKED_INIT_EVEVENTS_TIMEOUT,
    MOCKED_INIT_EVEVENTS_WARNING,
    MOCKED_SPARK_NEIGHBORS,
    MOCKED_SPARK_NEIGHBORS_ALIENS,
    MOCKED_SPARK_NEIGHBORS_ALL_ESTAB,
    MOCKED_SPARK_NEIGHBORS_DIFF_IDS,
    MOCKED_SPARK_NEIGHBORS_DIFF_IDS_ACCEPT_ALL,
    MOCKED_SPARK_NEIGHBORS_NO_ACCEPT,
    MOCKED_SPARK_NEIGHBORS_NO_ESTAB,
    SPARK_NEIGHBORS_OUTPUT,
    SPARK_NEIGHBORS_OUTPUT_JSON,
)


BASE_MODULE: str = spark.__name__
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_neighbors(self, mocked_openr_client: AsyncMock) -> None:
        # Set mock data for testing
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate(self, mocked_openr_client: AsyncMock) -> None:
        # Since print output uses neighbor command, we just validate if numbers are correct
        # Testing with one non ESTABLISHED neighbor node and one ESTABLISHED node
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_DEFAULT
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate_all_estab(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_DEFAULT
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate_no_estab(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_DEFAULT
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate_no_neighbors(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_DEFAULT
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

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate_init_event(self, mocked_openr_client: AsyncMock) -> None:
        # Checking when everything is good: NEIGHBOR_DISCOVERED is published and the duration is below warning
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_DEFAULT
        )
        mocked_returned_connection.getInitializationEvents.return_value = (
            MOCKED_INIT_EVENTS
        )
        mocked_returned_connection.getNeighbors.return_value = []

        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        init_event_pass_state = stdout_lines[2].split(" ")[-1]

        self.assertEqual("PASS", init_event_pass_state)

        # Checking when the NEIGHBORS_DISCOVEREd event isn't published
        mocked_returned_connection.getInitializationEvents.return_value = (
            MOCKED_INIT_EVEVENTS_NO_PUBLISH
        )
        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        error_msg_line = stdout_lines[3]
        init_event_pass_state = stdout_lines[2].split(" ")[-1]

        self.assertEqual("FAIL", init_event_pass_state)
        self.assertEqual("NEIGHBOR_DISCOVERED event is not published", error_msg_line)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate_regex_accept_all(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        # Checking passes when neighbors have different IDs
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_ACCEPT_ALL
        )
        mocked_returned_connection.getNeighbors.return_value = (
            MOCKED_SPARK_NEIGHBORS_DIFF_IDS_ACCEPT_ALL
        )

        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        regex_pass_state = stdout_lines[4].split(" ")[-1]
        regexes_line = stdout_lines[5]

        self.assertEqual("PASS", regex_pass_state)
        self.assertTrue(".*" in regexes_line)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate_regex_diff_ids(
        self, mocked_openr_client: AsyncMock
    ) -> None:

        # Checking if it passes with different ids and correct areas
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_ACCEPT_ALIENS_NODES
        )
        mocked_returned_connection.getNeighbors.return_value = (
            MOCKED_SPARK_NEIGHBORS_DIFF_IDS
        )
        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        regex_pass_state = stdout_lines[4].split(" ")[-1]
        self.assertEqual("PASS", regex_pass_state)

        # Checking if it passes with no neighbors
        mocked_returned_connection.getNeighbors.return_value = []
        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        regex_pass_state = stdout_lines[4].split(" ")[-1]
        self.assertEqual("PASS", regex_pass_state)

        # Checking if it fails with different ids and incorrect areas
        # Also checks if it correctly prints out only the failed neighbors
        mocked_returned_connection.getNeighbors.return_value = (
            MOCKED_SPARK_NEIGHBORS_ALIENS
        )
        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        regex_pass_state = stdout_lines[4].split(" ")[-1]
        failed_neighbor = stdout_lines[10]

        self.assertEqual("FAIL", regex_pass_state)
        self.assertTrue(
            "alien2" in failed_neighbor,
            f"Missing nodeName alien2 in failed neighbors string: \n {failed_neighbor}",
        )

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_spark_validate_regex_accept_none(
        self, mocked_openr_client: AsyncMock
    ) -> None:

        # Checking if it fails when no node is accepted
        # Also checks if it correclty prints out more than 1 failed neighbor
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_CONFIG_ACCEPT_NONE
        )
        mocked_returned_connection.getNeighbors.return_value = (
            MOCKED_SPARK_NEIGHBORS_NO_ACCEPT
        )
        invoked_return = self.runner.invoke(
            spark.SparkValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = self._parse_validate_stdout(invoked_return.stdout)
        regex_pass_state = stdout_lines[4].split(" ")[-1]
        failed_neighbors = stdout_lines[10] + stdout_lines[11]

        self.assertEqual("FAIL", regex_pass_state)
        self.assertTrue(
            "node2" in failed_neighbors,
            f"Missing nodeName node2 in failed neighbors string: \n {failed_neighbors}",
        )
        self.assertTrue(
            "alien5" in failed_neighbors,
            f"Missing nodeName alien5 in failed neighbors string: \n {failed_neighbors}",
        )
