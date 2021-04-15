#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.OpenrCtrl import OpenrCtrl


class GracefulRestartCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, yes: bool = False) -> None:
        question_str = "Are you sure to force sending GR msg to neighbors?"
        if not utils.yesno(question_str, yes):
            print()
            return

        client.floodRestartingMsg()
        print("Successfully forcing to send GR msgs.\n")
