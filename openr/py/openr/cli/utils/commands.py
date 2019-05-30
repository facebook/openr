#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import bunch
from openr.clients.openr_client import get_openr_ctrl_client
from openr.OpenrCtrl import OpenrCtrl


class OpenrCtrlCmd(object):
    """
    Command wrapping OpenrCtrl.Client
    """

    def __init__(self, cli_opts: bunch.Bunch) -> None:
        """ initialize the Config Store client """

        self.cli_opts = cli_opts  # type: bunch.Bunch
        self.enable_color = cli_opts.enable_color
        self.host = cli_opts.host
        self.timeout = cli_opts.timeout
        self.fib_agent_port = cli_opts.fib_agent_port

    def run(self, *args, **kwargs) -> None:
        """
        run method that invokes _run with client and arguments
        """

        with get_openr_ctrl_client(self.host, self.cli_opts) as client:
            self._run(client, *args, **kwargs)

    def _run(self, client: OpenrCtrl.Client, *args, **kwargs) -> None:
        """
        To be implemented by sub-command
        """

        raise NotImplementedError
