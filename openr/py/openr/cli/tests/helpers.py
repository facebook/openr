#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

"""File with common helper consts, function and mocks for unittests to use"""

from unittest.mock import MagicMock


BASE_CTRL_MODULE = "openr.OpenrCtrl.OpenrCtrl"
BASE_PY_CLIENT_MODULE = "openr.clients.openr_client"
BASE_UTILS_COMMANDS_MODULE = "openr.cli.utils.commands"
# pyre-fixme[5]: Global expression must be annotated.
COMMANDS_GET_OPENR_CTRL_CLIENT = f"{BASE_UTILS_COMMANDS_MODULE}.get_openr_ctrl_client"


def get_enter_thrift_magicmock(test_mocked_client: MagicMock) -> MagicMock:
    """We need to mock the context manager's enter with a common MagicMock
    so we can then patch what we want the thrift call to return within each test"""
    mocked_returned_connection = MagicMock()
    test_mocked_client.return_value.__enter__.return_value = mocked_returned_connection
    return mocked_returned_connection
