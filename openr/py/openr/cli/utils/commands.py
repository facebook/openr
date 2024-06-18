#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import asyncio
import json
import re
from typing import Any, Callable, Dict, List, Mapping, Optional, Set, Tuple

import bunch
import click
from openr.py.openr.clients.openr_client import get_openr_ctrl_cpp_client
from openr.thrift.KvStore.thrift_types import InitializationEvent, KeyDumpParams, Value
from openr.utils import printing
from openr.utils.consts import Consts
from thrift.python.client import ClientType


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

    def run(self, *args, **kwargs) -> int:
        """
        run method that invokes _run with client and arguments
        """

        async def _wrapper() -> int:
            ret_val: Optional[int] = 0
            async with get_openr_ctrl_cpp_client(
                self.host,
                self.cli_opts,
                client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
            ) as client:
                ret_val = await self._run(client, *args, **kwargs)
            if ret_val is None:
                ret_val = 0
            return ret_val

        return asyncio.run(_wrapper())

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

    async def _get_config(self) -> Dict[str, Any]:
        if self._config is None:
            async with get_openr_ctrl_cpp_client(self.host, self.cli_opts) as client:
                resp = await client.getRunningConfig()
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
        @param: dbs - decision_types.AdjDbs
        @param: nodes - set: the set of nodes for parsing
        @param: parse_func - function: the parsing function
        """
        for node, db in sorted(dbs.items()):
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

    def buildKvStoreKeyDumpParams(
        self,
        prefix: str = Consts.ALL_DB_MARKER,
        originator_ids: Optional[Set[str]] = None,
        keyval_hash: Optional[Dict[str, Value]] = None,
    ) -> KeyDumpParams:
        """
        Build KeyDumpParams based on input parameter list

        In thrift-python, the objects are immutable, and hence
        we should assign the attributes upon initialization
        """

        return KeyDumpParams(
            originatorIds=originator_ids,
            keyValHashes=keyval_hash,
            keys=[prefix] if prefix else None,
        )

    def validate_init_event(
        self,
        init_event_dict: Mapping[InitializationEvent, int],
        init_event: InitializationEvent,
    ) -> Tuple[bool, Optional[str]]:
        """
        Returns True if the init_event specified is published within it's defined time limit
        If init_event is published, returns the duration as a stylized string. If the checks fail,
        returns False along with an error message.
        """

        # Keeps track of whether or not the checks pass
        is_pass = True

        # If the check fails, this will hold the error msg string
        err_msg_str = None

        if init_event not in init_event_dict:
            is_pass = False
            err_msg_str = f"{init_event.name} event is not published"

        return is_pass, err_msg_str

    def pass_fail_str(self, is_pass: bool) -> str:
        """
        Returns a formatted pass or fail message
        """

        if is_pass:
            return click.style("PASS", bg="green", fg="black")
        else:
            return click.style("FAIL", bg="red", fg="black")

    def validation_result_str(
        self, module: str, check_title: str, is_pass: bool
    ) -> str:
        """
        Returns the label for a check as a stylized string
        Ex: [Spark] Regex Validation: PASS
        """

        result_str = click.style(
            f"[{module.title()}] {check_title.title()}: {self.pass_fail_str(is_pass)}",
            bold=True,
        )
        return result_str

    def print_initialization_event_check(
        self,
        is_pass: bool,
        err_msg_str: Optional[str],
        init_event: InitializationEvent,
        module: str,
    ) -> None:
        """
        Prints whether or not the initialization event passes or fails
        If it fails, outputs an error message
        If init_event is populated, outputs the time elapsed
        """

        click.echo(
            self.validation_result_str(module, "initialization event check", is_pass)
        )

        if err_msg_str:
            click.echo(err_msg_str)

    def validate_regexes(
        self, regexes: List[str], strings_to_check: List[str], expect_match: bool
    ) -> bool:
        """
        If expect_match is true, checks if all the strings in strings_to_check match atleast one of the regexes.
        Otherwise, checks if each string matches none of the regexes.
        """

        for string in strings_to_check:
            matches_regex = False

            for regex in regexes:
                if re.search(regex, string):
                    matches_regex = True

            if (expect_match and (not matches_regex)) or (
                (not expect_match) and matches_regex
            ):
                # None of the regexes match and the string must include one of them -> Fail
                # The string matches atleast one regex, but is supposed to match none of them -> Fail

                return False

        return True
