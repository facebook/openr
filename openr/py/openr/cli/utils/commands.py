#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
from typing import Any, Callable, Dict, Set, Optional

import bunch
from openr.clients.openr_client import get_openr_ctrl_client
from openr.Types import ttypes as openr_types
from openr.utils import printing
from openr.utils.consts import Consts


class OpenrCtrlCmd:
    """
    Command wrapping OpenrCtrl.Client
    """

    def __init__(self, cli_opts: Optional[bunch.Bunch] = None) -> None:
        """initialize the Config Store client"""
        if not cli_opts:
            cli_opts = bunch.Bunch()

        self.cli_opts: bunch.Bunch = cli_opts
        self.host = cli_opts.get("host") or Consts.DEFAULT_HOST
        self.timeout = cli_opts.get("timeout") or Consts.DEFAULT_TIMEOUT
        self.fib_agent_port = (
            cli_opts.get("fib_agent_port") or Consts.DEFAULT_FIB_AGENT_PORT
        )
        self._config = None

    def run(self, *args, **kwargs) -> Optional[int]:
        """
        run method that invokes _run with client and arguments
        """

        ret_val: Optional[int] = 0
        with get_openr_ctrl_client(self.host, self.cli_opts) as client:
            ret_val = self._run(client, *args, **kwargs)
            if ret_val is None:
                ret_val = 0
        return ret_val

    def _run(self, client: Any, *args, **kwargs) -> Any:
        """
        To be implemented by sub-command.
        @param: client - Client to connect to the Open/R server.
                         Set it to `Any` type here for the overridden method to choose the type in its parameter.
                         Currently, we have two types of the clients:
                                1. OpenrCtrl.Client for common APIs;
                                2. OpenrCtrlCpp client which implements stream APIs.
        """

        raise NotImplementedError

    def _get_config(self) -> Dict[str, Any]:
        if self._config is None:
            with get_openr_ctrl_client(self.host, self.cli_opts) as client:
                resp = client.getRunningConfig()
                self._config = json.loads(resp)
        return self._config

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

    def print_stats(self, stats_templates, counters):
        """
        Print in pretty format
        """

        suffixes = [".60", ".600", ".3600", ""]

        for template in stats_templates:
            counters_rows = []
            for title, key in template["counters"]:
                val = counters.get(key, None)
                counters_rows.append([title, "N/A" if not val and val != 0 else val])

            stats_cols = ["Stat", "1 min", "10 mins", "1 hour", "All Time"]
            stats_rows = []
            for title, key_prefix in template["stats"]:
                row = [title]
                for key in ["{}{}".format(key_prefix, s) for s in suffixes]:
                    val = counters.get(key, None)
                    row.append("N/A" if not val and val != 0 else val)
                stats_rows.append(row)

            if "title" in template:
                print("\n> {} ".format(template["title"]))

            if counters_rows:
                print()
                print(
                    printing.render_horizontal_table(
                        counters_rows, tablefmt="plain"
                    ).strip("\n")
                )
            if stats_rows:
                print()
                print(
                    printing.render_horizontal_table(
                        stats_rows, column_labels=stats_cols, tablefmt="simple"
                    ).strip("\n")
                )

    # common function used by decision, kvstore mnodule
    def buildKvStoreKeyDumpParams(
        self,
        prefix: str = Consts.ALL_DB_MARKER,
        originator_ids: Optional[Set[str]] = None,
        keyval_hash: Optional[Dict[str, openr_types.Value]] = None,
    ) -> openr_types.KeyDumpParams:
        """
        Build KeyDumpParams based on input parameter list
        """
        params = openr_types.KeyDumpParams(prefix)
        params.originatorIds = originator_ids if originator_ids else None
        params.keyValHashes = keyval_hash if keyval_hash else None
        if prefix:
            params.keys = [prefix]

        return params
