#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

"""File with common helper consts, function and mocks for unittests to use"""

from unittest.mock import AsyncMock, MagicMock


BASE_CTRL_MODULE = "openr.OpenrCtrl.OpenrCtrl"
BASE_UTILS_COMMANDS_MODULE = "openr.py.openr.cli.utils.commands"
KVSTORE_MODULE = "openr.cli.commands.kvstore"
COMMANDS_GET_OPENR_CTRL_CPP_CLIENT = (
    f"{BASE_UTILS_COMMANDS_MODULE}.get_openr_ctrl_cpp_client"
)
KVSTORE_GET_OPENR_CTRL_CPP_CLIENT = f"{KVSTORE_MODULE}.get_openr_ctrl_cpp_client"


def get_enter_thrift_magicmock(test_mocked_client: MagicMock) -> MagicMock:
    """We need to mock the context manager's enter with a common MagicMock
    so we can then patch what we want the thrift call to return within each test"""

    mocked_returned_connection = MagicMock()
    test_mocked_client.return_value.__enter__.return_value = mocked_returned_connection
    return mocked_returned_connection


def get_enter_thrift_asyncmock(test_mocked_client: AsyncMock) -> AsyncMock:
    """We need to mock the context manager's enter with a common AsyncMock
    so we can then patch what we want the thrift call to return within each test"""

    mocked_returned_connection = AsyncMock()
    test_mocked_client.return_value.__aenter__.return_value = mocked_returned_connection
    return mocked_returned_connection
