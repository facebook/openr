#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from json import loads
from tempfile import NamedTemporaryFile
from unittest.mock import MagicMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.cli.clis import config
from openr.cli.tests import helpers
from openr.OpenrCtrl.ttypes import OpenrError

from .fixtures import OPENR_CONFIG_STR


BASE_MODULE = "openr.cli.clis.config"


class CliConfigTests(TestCase):
    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            config.ConfigCli.config,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    def test_dryrun(self, mocked_openr_client: MagicMock) -> None:
        # Test we fail when exception is raised
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        mocked_returned_connection.dryrunConfig = MagicMock(
            side_effect=OpenrError("unittest")
        )
        bad_return = self.runner.invoke(
            config.ConfigDryRunCli.dryrun,
            ["/tmp/cooper_was_here_hello_there"],
            catch_exceptions=False,
        )
        # TODO: Make return values work - read config.py for more info
        self.assertEqual(0, bad_return.exit_code)

        # Write config to temporary file + return same config
        mocked_returned_connection.dryrunConfig = MagicMock(
            return_value=OPENR_CONFIG_STR
        )
        with NamedTemporaryFile("w") as ntf:
            ntf.write(OPENR_CONFIG_STR)
            ntf.close()
            invoked_return = self.runner.invoke(
                config.ConfigDryRunCli.dryrun,
                [ntf.name],
                catch_exceptions=False,
            )
            self.assertEqual(0, invoked_return.exit_code)

    # TODO: Handle bad return - Code does not today - We just spew exception
    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CLIENT)
    def test_show(self, mocked_openr_client: MagicMock) -> None:
        # Mock the thrift call used here
        mocked_returned_connection = helpers.get_enter_thrift_magicmock(
            mocked_openr_client
        )
        # We want `getRunningConfig` to return a string here
        # Use one we crafted from fixtures.py
        mocked_returned_connection.getRunningConfig.return_value = OPENR_CONFIG_STR

        # Run the command
        invoked_return = self.runner.invoke(
            config.ConfigShowCli.show,
            [],
            catch_exceptions=False,
        )

        # Check we get the config we want
        expected_conf = loads(OPENR_CONFIG_STR)
        self.assertTrue(
            expected_conf["areas"], loads(invoked_return.stdout_bytes)["areas"]
        )
