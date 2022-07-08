#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict
import copy
import re
from typing import Dict, List, Optional
from unittest.mock import MagicMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.cli.clis import kvstore
from openr.cli.tests import helpers
from openr.KvStore import ttypes as kvstore_types

BASE_MODULE = "openr.cli.clis.kvstore"
BASE_CMD_MODULE = "openr.cli.commands.kvstore"

from .fixtures import (
    AreaId,
    MOCKED_KVSTORE_PEERS_DIFF_STATES,
    MOCKED_KVSTORE_PEERS_ONE_FAIL,
    MOCKED_KVSTORE_PEERS_ONE_PEER,
    MOCKED_KVSTORE_PEERS_TWO_PEERS,
    MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS,
)


class CliKvStoreTests(TestCase):
    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            kvstore.KvStoreCli.kvstore,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    def _test_kvstore_peers_helper(
        self,
        peer_lines: List[str],
        expected_peers: Dict[str, Dict[str, kvstore_types.PeerSpec]],
    ) -> None:
        """
        Parses stdout to check if the information outputed and number of peers
        matches the expected peers.
        """

        peer_lines = [line for line in peer_lines if line != ""]
        num_expected_peers = 0
        for _, peer_areas in expected_peers.items():
            num_expected_peers += len(peer_areas.keys())

        self.assertEqual(
            len(peer_lines),
            num_expected_peers,
            f"Expected number of peers: {num_expected_peers}, got {len(peer_lines)}",
        )

        for peer in peer_lines:
            peer = [token for token in peer.split(" ") if token != ""]

            actual_peer_name = peer[0]
            actual_state = peer[1]
            actual_address = peer[2]
            actual_port = peer[3]
            actual_area = peer[4]

            self.assertTrue(
                actual_area in expected_peers, f"Unexpected area, {actual_area}"
            )
            self.assertTrue(
                actual_peer_name in expected_peers[actual_area],
                f"Unexpected peer name, {actual_peer_name}",
            )

            expected_peer = expected_peers[actual_area][actual_peer_name]

            self.assertEqual(
                actual_address,
                expected_peer.peerAddr,
                f"Expected address: {expected_peer.peerAddr}, got {actual_address}",
            )
            expected_peer_state = kvstore_types.KvStorePeerState._VALUES_TO_NAMES[
                expected_peer.state
            ]
            self.assertEqual(
                actual_state,
                expected_peer_state,
                f"Expected peer state: {expected_peer_state}, got {actual_state}",
            )
            self.assertEqual(
                actual_port,
                str(expected_peer.ctrlPort),
                f"Expected ctrlport: {expected_peer.ctrlPort}, got {actual_port}",
            )

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CLIENT)
    def test_kvstore_peers(self, mocked_openr_client: MagicMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS
        )

        expected_peers = {
            AreaId.AREA1.value: MOCKED_KVSTORE_PEERS_TWO_PEERS,
            AreaId.AREA2.value: MOCKED_KVSTORE_PEERS_ONE_PEER,
            AreaId.AREA3.value: {},
        }

        mocked_returned_connection.getKvStorePeersArea.side_effect = (
            lambda area_id: expected_peers[area_id]
        )

        invoked_return = self.runner.invoke(
            kvstore.PeersCli.peers,
            [],
            catch_exceptions=False,
        )

        peer_lines = invoked_return.stdout.split("\n")[5:]
        self._test_kvstore_peers_helper(peer_lines, expected_peers)

    def _check_validation_state(
        self, pass_expected: bool, validation_state_line: str
    ) -> None:
        """
        Helper for kvstore validate unit tests, takes in a line containing the validation state
        and checks if the actual validation state matches the expected one
        """

        validation_state = validation_state_line.split(" ")[-1]
        pass_str = "PASS" if pass_expected else "FAIL"

        self.assertEqual(
            validation_state,
            pass_str,
            f"Expected the check to {pass_str}, instead the check {validation_state}ed",
        )

    def _find_validation_string(
        self, stdout_lines: List[str], start_idx: int
    ) -> Optional[int]:
        """
        A validation string is the title of each check in the format:
        [Module Name] Name of Check: Pass state
        Returns the index of the first validation string found. Ex with start_idx = 0:
        Stdout:
        [Kvstore] Local Node Advertising Atleast One Adjaceny And One Prefix Key Check: PASS
        [Kvstore] Peer State Check: PASS
        Would return 0.
        If there is no next validation string, returns None
        """

        for (idx, line) in enumerate(stdout_lines, start_idx):
            if re.match("[Kvstore]", line):
                return idx

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CLIENT)
    def test_kvstore_validate_peer_state_pass(
        self, mocked_openr_client: MagicMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS
        )

        expected_peers_all_pass = {
            AreaId.AREA1.value: MOCKED_KVSTORE_PEERS_TWO_PEERS,
            AreaId.AREA2.value: MOCKED_KVSTORE_PEERS_ONE_PEER,
            AreaId.AREA3.value: {},
        }

        mocked_returned_connection.getKvStorePeersArea.side_effect = (
            lambda area_id: expected_peers_all_pass[area_id]
        )

        invoked_return = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        pass_line = stdout_lines[3]
        self._check_validation_state(
            True, pass_line
        )  # True implies we expect this check to pass

    def _test_kvstore_validate_peer_state_helper(
        self, stdout: str, invalid_peers: Dict[str, Dict[str, kvstore_types.PeerSpec]]
    ) -> None:
        """
        Helper to check for the failure case of validate peers
        Checks if the peers outputted are correct, and the validate state to be fail
        """

        stdout_lines = stdout.split("\n")
        pass_line_idx = 3
        pass_line = stdout_lines[pass_line_idx]
        self._check_validation_state(
            False, pass_line
        )  # False implies we expect this check to fail

        # Hard coding the ending line of the peers could prevent us from catching
        # duplicate peers or valid peers being printed out
        next_validation_string_idx = self._find_validation_string(
            stdout_lines, pass_line_idx + 1
        )  # Offset by 1 so the next validation string idx is returned

        if next_validation_string_idx:
            last_peer_idx = next_validation_string_idx - 1
        else:
            last_peer_idx = len(stdout_lines)

        invalid_peer_lines = stdout_lines[10:last_peer_idx]
        self._test_kvstore_peers_helper(invalid_peer_lines, invalid_peers)

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CLIENT)
    def test_kvstore_validate_peer_state_fail(
        self, mocked_openr_client: MagicMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS
        )

        # Testing when some of the peers will fail the check
        expected_peers_some_fail = {
            AreaId.AREA1.value: MOCKED_KVSTORE_PEERS_DIFF_STATES,
            AreaId.AREA2.value: MOCKED_KVSTORE_PEERS_ONE_PEER,
            AreaId.AREA3.value: MOCKED_KVSTORE_PEERS_ONE_FAIL,
        }

        mocked_returned_connection.getKvStorePeersArea.side_effect = (
            lambda area_id: expected_peers_some_fail[area_id]
        )

        invoked_return_some_fail = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        # MOCKED_DIFF_STATE_INVALID_PEERS will be used later, so it's useful to have it as its own object
        MOCKED_DIFF_STATE_INVALID_PEERS = copy.deepcopy(
            MOCKED_KVSTORE_PEERS_DIFF_STATES
        )
        del MOCKED_DIFF_STATE_INVALID_PEERS["node6"]

        expected_peers_some_fail[AreaId.AREA1.value] = MOCKED_DIFF_STATE_INVALID_PEERS
        expected_peers_some_fail[AreaId.AREA2.value] = {}
        expected_peers_all_fail = expected_peers_some_fail

        self._test_kvstore_validate_peer_state_helper(
            invoked_return_some_fail.stdout, expected_peers_all_fail
        )

        # Testing when all of the peers will fail the check
        mocked_returned_connection.getKvStorePeersArea.side_effect = (
            lambda area_id: expected_peers_all_fail[area_id]
        )

        invoked_return_all_fail = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        self._test_kvstore_validate_peer_state_helper(
            invoked_return_all_fail.stdout, expected_peers_all_fail
        )
