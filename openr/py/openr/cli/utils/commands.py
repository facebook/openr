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

    def __init__(self, options: bunch.Bunch) -> None:
        """ initialize the Config Store client """

        self._options = options  # type: bunch.Bunch

    def run(self, *args, **kwargs) -> None:
        """
        run method that invokes _run with client and arguments
        """

        with get_openr_ctrl_client(self._options.host, self._options) as client:
            self._run(client, *args, **kwargs)

    def _run(self, client: OpenrCtrl.Client, *args, **kwargs) -> None:
        """
        To be implemented by sub-command
        """

        raise NotImplementedError
