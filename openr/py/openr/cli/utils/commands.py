#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import asyncio
import json
import re
from typing import Any, Callable, Dict, List, Mapping, Optional, Set, Tuple

import bunch
import click
from openr.clients.openr_client import (
    get_openr_ctrl_client_py,
    get_openr_ctrl_cpp_client,
)
from openr.KvStore import ttypes as kv_store_types
from openr.thrift.KvStore.thrift_types import (
    InitializationEvent,
    InitializationEventTimeDuration,
    InitializationEventTimeLabels,
    KeyDumpParams,
    Publication,
    Value,
)
from openr.utils import printing
from openr.utils.consts import Consts
from thrift.python.client import ClientType

# to be deprecated
class OpenrCtrlCmdPy:
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

        ret_val: Optional[int] = 0
        with get_openr_ctrl_client_py(self.host, self.cli_opts) as client:
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

    def _get_config_py(self) -> Dict[str, Any]:
        if self._config is None:
            with get_openr_ctrl_client_py(self.host, self.cli_opts) as client:
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
        @param: dbs - decision_types.AdjDbs
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

    # common function used by decision, kvstore module
    def buildKvStoreKeyDumpParamsPy(
        self,
        prefix: str = Consts.ALL_DB_MARKER,
        originator_ids: Optional[Set[str]] = None,
        keyval_hash: Optional[Dict[str, kv_store_types.Value]] = None,
    ) -> kv_store_types.KeyDumpParams:
        """
        Build KeyDumpParams based on input parameter list
        """
        params = kv_store_types.KeyDumpParams(prefix)
        params.originatorIds = originator_ids if originator_ids else None
        params.keyValHashes = keyval_hash if keyval_hash else None
        if prefix:
            params.keys = [prefix]

        return params

    def fetch_initialization_events_py(
        self, client: Any
    ) -> Dict[kv_store_types.InitializationEvent, int]:
        """
        Fetch Initialization events as a dictionary via thrift call
        """

        return client.getInitializationEvents()

    def fetch_running_config_thrift(self, client: Any) -> Any:
        """
        Fetch the current running config via thrift call
        """

        return client.getRunningConfigThrift()

    def fetch_kvstore_peers(self, client: Any, area=None) -> Any:
        """
        Fetch a dictionary of {peer name : peer spec} via thrift call
        """

        if area:
            return client.getKvStorePeersArea(area)

        return client.getKvStorePeers()

    def fetch_keyvals_py(
        self,
        client: Any,
        areas: Set[Any],
        keyDumpParams: kv_store_types.KeyDumpParams,
    ) -> Dict[str, kv_store_types.Publication]:
        """
        Fetch the keyval publication for each area specified in areas via thrift call
        Returned as a dict {area : publication}
        If keyDumpParams is specified, returns keyvals filtered accordingly
        """

        area_to_publication_dict = {}
        for area in areas:
            area_to_publication_dict[area] = client.getKvStoreKeyValsFilteredArea(
                keyDumpParams, area
            )
        return area_to_publication_dict

    def validate_init_event_py(
        self,
        init_event_dict: Dict[kv_store_types.InitializationEvent, int],
        init_event: kv_store_types.InitializationEvent,
    ) -> Tuple[bool, Optional[str], Optional[str]]:
        """
        Returns True if the init_event specified is published within it's defined time limit
        If init_event is published, returns the duration as a stylized string. If the checks fail,
        returns False along with an error message.
        """

        # Keeps track of whether or not the checks pass
        is_pass = True

        # If the check fails, this will hold the error msg string
        err_msg_str = None

        # If the init_event is published, stores the duration as a stylized string
        dur_str = None

        # TODO: (klu02) Update the client to thrift.py3 so we can use thrift.py3 types instead of legacy
        init_event_name = kv_store_types.InitializationEvent._VALUES_TO_NAMES[
            init_event
        ]

        if init_event not in init_event_dict:
            is_pass = False
            err_msg_str = f"{init_event_name} event is not published"
        else:
            warning_label = InitializationEventTimeLabels[
                f"{init_event_name}_WARNING_MS"
            ]
            timeout_label = InitializationEventTimeLabels[
                f"{init_event_name}_TIMEOUT_MS"
            ]

            warning_time = InitializationEventTimeDuration[warning_label]
            timeout_time = InitializationEventTimeDuration[timeout_label]

            init_event_dur = init_event_dict[init_event]

            if init_event_dur < warning_time:
                dur_str = click.style(str(init_event_dur), fg="green")
            elif init_event_dur < timeout_time:
                dur_str = click.style(str(init_event_dur), fg="yellow")
            else:
                dur_str = click.style(str(init_event_dur), fg="red")
                err_msg_str = f"{init_event_name} event duration exceeds acceptable time limit (>{timeout_time}ms)"
                is_pass = False

        return is_pass, err_msg_str, dur_str

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
        dur_str: Optional[str],
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

        if dur_str:
            click.echo(
                f"Time elapsed for event, {init_event.name}, since Open/R started: {dur_str}ms"
            )

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


class OpenrCtrlCmd(OpenrCtrlCmdPy):
    # @override
    def run(self, *args, **kwargs) -> int:
        """
        run method that invokes _run with client and arguments
        """

        async def _wrapper() -> int:
            async with get_openr_ctrl_cpp_client(
                self.host,
                self.cli_opts,
                client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
            ) as client:
                await self._run(client, *args, **kwargs)
            return 0

        return asyncio.run(_wrapper())

    async def _get_config(self) -> Dict[str, Any]:
        if self._config is None:
            async with get_openr_ctrl_cpp_client(self.host, self.cli_opts) as client:
                resp = await client.getRunningConfig()
                self._config = json.loads(resp)
        return self._config

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
            prefix=prefix,
            originatorIds=originator_ids,
            keyValHashes=keyval_hash,
            keys=[prefix] if prefix else None,
        )

    def fetch_keyvals(
        self,
        client: Any,
        areas: Set[Any],
        keyDumpParams: KeyDumpParams,
    ) -> Dict[str, Publication]:
        """
        Fetch the keyval publication for each area specified in areas via thrift call
        Returned as a dict {area : publication}
        If keyDumpParams is specified, returns keyvals filtered accordingly
        """

        area_to_publication_dict = {}
        for area in areas:
            area_to_publication_dict[area] = client.getKvStoreKeyValsFilteredArea(
                keyDumpParams, area
            )
        return area_to_publication_dict

    def validate_init_event(
        self,
        init_event_dict: Mapping[InitializationEvent, int],
        init_event: InitializationEvent,
    ) -> Tuple[bool, Optional[str], Optional[str]]:
        """
        Returns True if the init_event specified is published within it's defined time limit
        If init_event is published, returns the duration as a stylized string. If the checks fail,
        returns False along with an error message.
        """

        # Keeps track of whether or not the checks pass
        is_pass = True

        # If the check fails, this will hold the error msg string
        err_msg_str = None

        # If the init_event is published, stores the duration as a stylized string
        dur_str = None

        if init_event not in init_event_dict:
            is_pass = False
            err_msg_str = f"{init_event.name} event is not published"
        else:
            warning_label = InitializationEventTimeLabels[
                f"{init_event.name}_WARNING_MS"
            ]
            timeout_label = InitializationEventTimeLabels[
                f"{init_event.name}_TIMEOUT_MS"
            ]

            warning_time = InitializationEventTimeDuration[warning_label]
            timeout_time = InitializationEventTimeDuration[timeout_label]

            init_event_dur = init_event_dict[init_event]

            if init_event_dur < warning_time:
                dur_str = click.style(str(init_event_dur), fg="green")
            elif init_event_dur < timeout_time:
                dur_str = click.style(str(init_event_dur), fg="yellow")
            else:
                dur_str = click.style(str(init_event_dur), fg="red")
                err_msg_str = f"{init_event.name} event duration exceeds acceptable time limit (>{timeout_time}ms)"
                is_pass = False

        return is_pass, err_msg_str, dur_str
