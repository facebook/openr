#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict
from typing import Dict, List
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
