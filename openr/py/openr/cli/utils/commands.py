#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


from typing import Any, Callable, Dict, List, Optional

import bunch
from openr.clients.openr_client import get_openr_ctrl_client
from openr.OpenrCtrl import OpenrCtrl
from openr.Types import ttypes as openr_types
from openr.utils.consts import Consts


class OpenrCtrlCmd(object):
    """
    Command wrapping OpenrCtrl.Client
    """

    def __init__(self, cli_opts: bunch.Bunch) -> None:
        """ initialize the Config Store client """

        self.cli_opts = cli_opts  # type: bunch.Bunch
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

    def iter_dbs(
        self,
        container: Any,
        dbs: Dict,
        nodes: set,
        parse_func: Callable[[Any, Dict], None],
    ):
        """
        parse prefix/adj databases

        @param: container - container to store the generated data and returns
        @param: dbs - decision_types.PrefixDbs or decision_types.AdjDbs
        @param: nodes - set: the set of nodes for parsing
        @param: parse_func - function: the parsing function
        """
        for (node, db) in sorted(dbs.items()):
            if "all" not in nodes and node not in nodes:
                continue
            parse_func(container, db)

    # common function used by decision, kvstore mnodule
    def buildKvStoreKeyDumpParams(
        self,
        prefix: str = Consts.ALL_DB_MARKER,
        originator_ids: Optional[List[str]] = None,
        keyval_hash: Optional[Dict[str, openr_types.Value]] = None,
    ) -> openr_types.KeyDumpParams:
        """
        Build KeyDumpParams based on input parameter list
        """
        params = openr_types.KeyDumpParams(prefix)
        params.originatorIds = []
        params.keyValHashes = None
        if prefix:
            params.keys = [prefix]

        if originator_ids:
            params.originatorIds = originator_ids
        if keyval_hash:
            params.keyValHashes = keyval_hash

        return params
