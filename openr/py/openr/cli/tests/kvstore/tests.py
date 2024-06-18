#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict
import copy
import datetime
import re
from typing import Dict, List, Optional
from unittest.mock import AsyncMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.py.openr.cli.clis import kvstore
from openr.py.openr.cli.tests import helpers
from openr.py.openr.utils.printing import sprint_bytes
from openr.thrift.KvStore import thrift_types as kvstore_types
from openr.thrift.OpenrConfig.thrift_types import OpenrConfig

BASE_MODULE: str = kvstore.__name__
BASE_CMD_MODULE = "openr.py.openr.cli.commands.kvstore"

from .fixtures import (
    AreaId,
    MOCKED_INIT_EVENTS_PASS,
    MOCKED_KVSTORE_PEERS_DIFF_STATES,
    MOCKED_KVSTORE_PEERS_ONE_FAIL,
    MOCKED_KVSTORE_PEERS_ONE_PEER,
    MOCKED_KVSTORE_PEERS_TWO_PEERS,
    MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS,
    MOCKED_THRIFT_CONFIG_ONE_AREA,
    MOCKED_THRIFT_CONFIG_ONE_AREA_HIGH_TTL,
    MockedInvalidKeyVals,
    MockedKeys,
    MockedValidKeyVals,
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
            expected_peer_state = expected_peer.state.name
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

    def _test_keys_helper(
        self,
        keyvals: List[str],
        expected_keys: Dict[str, Dict[str, kvstore_types.Value]],
    ) -> None:
        """
        Checks if the information for each key in string form matches the expected values
        in expected_keys
        """

        keyvals = [kv for kv in keyvals if kv != ""]
        num_expected_keyvals = 0
        for _, keyvals_of_area in expected_keys.items():
            num_expected_keyvals += len(keyvals_of_area.keys())

        self.assertEqual(
            len(keyvals),
            num_expected_keyvals,
            f"Expected number of KeyVals: {num_expected_keyvals}, got {len(keyvals)}",
        )

        for keyval in keyvals:
            keyval = [token for token in keyval.split(" ") if token != ""]

            actual_key = keyval[0]
            actual_originator = keyval[1]
            actual_ver = keyval[2]
            actual_hash = keyval[3]
            # The size is formatted as [num of KB] KB"
            # We have to account for the space when tokenizing
            actual_size = f"{keyval[4]} {keyval[5]}"
            actual_area = keyval[6]
            actual_ttl = keyval[7]
            actual_ttl_version = keyval[9]

            self.assertTrue(
                actual_area in expected_keys, f"Unexpected area, {actual_area}"
            )
            self.assertTrue(
                actual_key in expected_keys[actual_area],
                f"Unexpected key, {actual_key}",
            )

            expected_keyval = expected_keys[actual_area][actual_key]

            self.assertEqual(
                actual_ver,
                str(expected_keyval.version),
                f"Expected Version: {expected_keyval.version}, got: {actual_ver}",
            )

            self.assertEqual(
                actual_originator,
                expected_keyval.originatorId,
                f"Expected Originator ID: {expected_keyval.originatorId}, got: {actual_originator}",
            )

            expected_ttl = str(datetime.timedelta(milliseconds=expected_keyval.ttl))
            self.assertEqual(
                actual_ttl,
                expected_ttl,
                f"Expected TTL: {expected_ttl}, got: {actual_ttl}",
            )

            self.assertEqual(
                actual_ttl_version,
                str(expected_keyval.ttlVersion),
                f"Expected TTL Version: {expected_keyval.ttlVersion}, got: {actual_ttl_version}",
            )

            if expected_keyval.hash is not None:
                hash_sign = "+" if expected_keyval.hash > 0 else ""
                expected_hash = f"{hash_sign}{expected_keyval.hash:x}"

                self.assertEqual(
                    actual_hash,
                    expected_hash,
                    f"Expected Hash: {expected_hash} got: {actual_hash}",
                )

            value_size = len(
                expected_keyval.value if expected_keyval.value is not None else b""
            )
            expected_size = sprint_bytes(
                32 + len(actual_key) + len(expected_keyval.originatorId) + value_size
            )
            self.assertEqual(
                actual_size,
                expected_size,
                f"Expected Size: {expected_size} got: {actual_size}",
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

        for idx, line in enumerate(stdout_lines[start_idx:], start_idx):
            if re.match(r"\[Kvstore\]", line):
                return idx

    def _get_keyval_lines(
        self, stdout_lines: List[str], validation_str_idx: int
    ) -> List[str]:
        """
        Returns the list of lines with keyval info given the index of the
        validation string of the current check
        """

        key_start_idx = None
        for idx, line in enumerate(
            stdout_lines[validation_str_idx:], validation_str_idx
        ):
            if re.match("Key", line):
                key_start_idx = idx + 2

        self.assertTrue(key_start_idx is not None, "No information about keys found")

        next_validation_str_idx = self._find_validation_string(
            stdout_lines, key_start_idx
        )
        if next_validation_str_idx is None:
            next_validation_str_idx = len(stdout_lines)

        # The last keyval is printed on the line before the next validation str
        return stdout_lines[key_start_idx:next_validation_str_idx]

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_peers(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
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

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_check_key_advertising_and_ttl_pass(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS
        )
        mocked_returned_connection.getKvStorePeersArea.return_value = {}

        area1_publication = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedValidKeyVals.VAL1.value,
                MockedKeys.PREF1.value: MockedValidKeyVals.VAL2.value,
                MockedKeys.PREF2.value: MockedValidKeyVals.VAL3.value,
            }
        )
        area2_publication = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedValidKeyVals.VAL3.value,
                MockedKeys.PREF3.value: MockedValidKeyVals.VAL5.value,
            }
        )
        area3_publication = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedValidKeyVals.VAL4.value,
                MockedKeys.PREF3.value: MockedValidKeyVals.VAL3.value,
            }
        )

        expected_publications = {
            AreaId.AREA1.value: area1_publication,
            AreaId.AREA2.value: area2_publication,
            AreaId.AREA3.value: area3_publication,
        }

        mocked_returned_connection.getKvStoreKeyValsFilteredArea.side_effect = (
            lambda _, area: expected_publications[area]
        )

        invoked_return = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        # Example of stdout for all checks passing:
        # [Kvstore] Local Node Advertising Atleast One Adjaceny And One Prefix Key Check: PASS
        # [Kvstore] Peer State Check: PASS
        # [Kvstore] Key Ttl Check: PASS
        # Current Configured Max TTL: 3600000 ms

        stdout_lines = invoked_return.stdout.split("\n")
        pass_line_advertising_check = stdout_lines[0]
        self._check_validation_state(
            True, pass_line_advertising_check
        )  # True implies we expect this check to pass

        pass_line_ttl_check = stdout_lines[2]
        self._check_validation_state(
            True, pass_line_ttl_check
        )  # True implies we expect this check to pass

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_check_key_advertising_fail(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_ONE_AREA
        )
        mocked_returned_connection.getKvStorePeersArea.return_value = {}

        # Check if the check fails if no prefix key is being advertised
        area1_publication_no_pref = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedValidKeyVals.VAL1.value,
            }
        )
        expected_publications_no_pref = {AreaId.AREA1.value: area1_publication_no_pref}

        mocked_returned_connection.getKvStoreKeyValsFilteredArea.side_effect = (
            lambda _, area: expected_publications_no_pref[area]
        )

        invoked_return_no_pref = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return_no_pref.stdout.split("\n")
        pass_line = stdout_lines[0]
        self._check_validation_state(
            False, pass_line
        )  # False implies we expect this check to fail
        adj_key_advertisement_state = stdout_lines[1].split(" ")[-1]
        pref_key_advertisement_state = stdout_lines[2].split(" ")[-1]

        self.assertEqual("PASS", adj_key_advertisement_state)
        self.assertEqual("FAIL", pref_key_advertisement_state)

        # Check if the check fails if no adj key is being advertised
        area1_publication_no_adj = kvstore_types.Publication(
            keyVals={
                MockedKeys.PREF1.value: MockedValidKeyVals.VAL1.value,
            }
        )
        expected_publications_no_adj = {AreaId.AREA1.value: area1_publication_no_adj}

        mocked_returned_connection.getKvStoreKeyValsFilteredArea.side_effect = (
            lambda _, area: expected_publications_no_adj[area]
        )

        invoked_return_no_adj = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return_no_adj.stdout.split("\n")
        pass_line = stdout_lines[0]
        self._check_validation_state(
            False, pass_line
        )  # False implies we expect this check to fail
        adj_key_advertisement_state = stdout_lines[1].split(" ")[-1]
        pref_key_advertisement_state = stdout_lines[2].split(" ")[-1]

        self.assertEqual("FAIL", adj_key_advertisement_state)
        self.assertEqual("PASS", pref_key_advertisement_state)

        # Check if the check fails if no key is being advertised
        area1_publication_no_key = kvstore_types.Publication(keyVals={})
        expected_publications_no_key = {AreaId.AREA1.value: area1_publication_no_key}
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.side_effect = (
            lambda _, area: expected_publications_no_key[area]
        )

        invoked_return_no_key = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return_no_key.stdout.split("\n")
        pass_line = stdout_lines[0]
        self._check_validation_state(
            False, pass_line
        )  # False implies we expect this check to fail
        adj_key_advertisement_state = stdout_lines[1].split(" ")[-1]
        pref_key_advertisement_state = stdout_lines[2].split(" ")[-1]

        self.assertEqual("FAIL", adj_key_advertisement_state)
        self.assertEqual("FAIL", pref_key_advertisement_state)

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_validate_peer_state_pass(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS
        )
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.return_value = (
            kvstore_types.Publication()
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
        self._check_validation_state(True, pass_line)

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
        self._check_validation_state(False, pass_line)
        # Hard coding the ending line of the peers could prevent us from catching
        # duplicate peers or valid peers being printed out
        next_validation_string_idx = self._find_validation_string(
            stdout_lines, pass_line_idx + 1
        )  # Offset by 1 so the next validation string idx is returned

        if next_validation_string_idx:
            last_peer_idx = next_validation_string_idx
        else:
            last_peer_idx = len(stdout_lines)

        invalid_peer_lines = stdout_lines[10:last_peer_idx]
        self._test_kvstore_peers_helper(invalid_peer_lines, invalid_peers)

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_validate_peer_state_fail(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS
        )
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.return_value = (
            kvstore_types.Publication()
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

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_check_key_ttl_fail(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS
        )
        mocked_returned_connection.getKvStorePeersArea.return_value = {}

        area1_publication = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedInvalidKeyVals.VAL1.value,
                MockedKeys.PREF1.value: MockedValidKeyVals.VAL2.value,
                MockedKeys.PREF2.value: MockedInvalidKeyVals.VAL2.value,
            }
        )
        area2_publication = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedValidKeyVals.VAL3.value,
                MockedKeys.PREF3.value: MockedValidKeyVals.VAL5.value,
            }
        )
        area3_publication = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedValidKeyVals.VAL4.value,
                MockedKeys.PREF3.value: MockedInvalidKeyVals.VAL2.value,
            }
        )

        expected_publications = {
            AreaId.AREA1.value: area1_publication,
            AreaId.AREA2.value: area2_publication,
            AreaId.AREA3.value: area3_publication,
        }

        mocked_returned_connection.getKvStoreKeyValsFilteredArea.side_effect = (
            lambda _, area: expected_publications[area]
        )

        invoked_return = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        validation_str_idx = 2
        validation_str = stdout_lines[2]
        self._check_validation_state(False, validation_str)
        print(invoked_return.stdout)
        keyval_lines = self._get_keyval_lines(stdout_lines, validation_str_idx)
        invalid_keys = {
            AreaId.AREA1.value: {
                MockedKeys.ADJ1.value: MockedInvalidKeyVals.VAL1.value,
                MockedKeys.PREF2.value: MockedInvalidKeyVals.VAL2.value,
            },
            AreaId.AREA3.value: {
                MockedKeys.PREF3.value: MockedInvalidKeyVals.VAL2.value,
            },
        }

        self._test_keys_helper(keyval_lines, invalid_keys)

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_check_key_ttl_high_ttl(
        self, mocked_openr_client: AsyncMock
    ) -> None:
        # Check with a different configured ttl
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = (
            MOCKED_THRIFT_CONFIG_ONE_AREA_HIGH_TTL
        )
        mocked_returned_connection.getKvStorePeersArea.return_value = {}

        area1_publication = kvstore_types.Publication(
            keyVals={
                MockedKeys.ADJ1.value: MockedValidKeyVals.VAL6.value,
                MockedKeys.PREF1.value: MockedInvalidKeyVals.VAL3.value,
            }
        )

        expected_publications = {AreaId.AREA1.value: area1_publication}
        mocked_returned_connection.getKvStoreKeyValsFilteredArea.side_effect = (
            lambda _, area: expected_publications[area]
        )

        invoked_return = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        validation_str_idx = 2
        validation_str = stdout_lines[2]
        self._check_validation_state(False, validation_str)

        keyval_lines = self._get_keyval_lines(stdout_lines, validation_str_idx)
        invalid_key = {
            AreaId.AREA1.value: {
                MockedKeys.PREF1.value: MockedInvalidKeyVals.VAL3.value
            }
        }
        self._test_keys_helper(keyval_lines, invalid_key)

    @patch(helpers.KVSTORE_GET_OPENR_CTRL_CPP_CLIENT)
    def test_kvstore_check_init_event(self, mocked_openr_client: AsyncMock) -> None:
        # Check with a different configured ttl
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getInitializationEvents.return_value = (
            MOCKED_INIT_EVENTS_PASS
        )
        mocked_returned_connection.getRunningConfigThrift.return_value = OpenrConfig()

        invoked_return = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        # This check appears on line 6 of stdout in this case
        validation_str = stdout_lines[6]

        self._check_validation_state(True, validation_str)
        pass_time = MOCKED_INIT_EVENTS_PASS[
            kvstore_types.InitializationEvent.KVSTORE_SYNCED
        ]

        # Check fail - event is not published
        mocked_returned_connection.getInitializationEvents.return_value = {}

        invoked_return = self.runner.invoke(
            kvstore.ValidateCli.validate,
            [],
            catch_exceptions=False,
        )

        stdout_lines = invoked_return.stdout.split("\n")
        validation_str = stdout_lines[6]
        error_str = stdout_lines[7]

        self._check_validation_state(False, validation_str)
        self.assertEqual("KVSTORE_SYNCED event is not published", error_str)
