#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from click.testing import CliRunner
from later.unittest import TestCase
from openr.cli.clis.openr import OpenrCli, VersionCli


class CliOpenrTests(TestCase):
    async def test_help(self) -> None:
        runner = CliRunner()
        invoked_return = runner.invoke(
            OpenrCli.openr,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    def test_cli_objects(self) -> None:
        self.assertTrue(OpenrCli())
        self.assertTrue(VersionCli())
