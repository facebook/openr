#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import re
from typing import Any, Callable, Tuple

import bunch
import click
import zmq
from openr.cli.utils.default_option_overrides import getDefaultOption
from openr.Platform import ttypes as platform_types
from openr.utils.consts import Consts


OPTIONS = bunch.Bunch(
    {
        "client_id": platform_types.FibClient.OPENR,
        "color": True,
        "config_store_url": Consts.CONFIG_STORE_URL,
        "decision_rep_port": Consts.DECISION_REP_PORT,
        "enable_color": True,
        "fib_agent_port": Consts.FIB_AGENT_PORT,
        "fib_rep_port": Consts.FIB_REP_PORT,
        "health_checker_cmd_port": Consts.HEALTH_CHECKER_CMD_PORT,
        "host": "localhost",
        "kv_pub_port": Consts.KVSTORE_PUB_PORT,
        "kv_rep_port": Consts.KVSTORE_REP_PORT,
        "lm_cmd_port": Consts.LINK_MONITOR_CMD_PORT,
        "monitor_pub_port": Consts.MONITOR_PUB_PORT,
        "monitor_rep_port": Consts.MONITOR_REP_PORT,
        "prefix_mgr_cmd_port": Consts.PREFIX_MGR_CMD_PORT,
        "proto_factory": Consts.PROTO_FACTORY,
        "ssl": True,
        "timeout": Consts.TIMEOUT_MS,
        "verbose": False,
        "zmq_ctx": zmq.Context(),
    }
)


def nameFromOpt(opt: str) -> str:
    return re.sub("^-+", "", opt.split("/")[0]).replace("-", "_")


def getNameFromOpts(opts: Tuple[Any, ...]) -> str:
    names = [nameFromOpt(n) for n in opts if nameFromOpt(n) in OPTIONS]
    assert len(names) == 1, "Exaclty one parameter must correspond to an option"
    return names[0]


def set_option(ctx, param, value) -> None:
    name = getNameFromOpts(param.opts)
    OPTIONS[name] = value if value is not None else getDefaultOption(OPTIONS, name)


def breeze_option(*args: Any, **kwargs: Any) -> Callable[[Any], Any]:
    assert "default" not in kwargs and getNameFromOpts(args) in OPTIONS
    assert "callback" not in kwargs
    kwargs["default"] = None
    kwargs["callback"] = set_option
    return click.option(*args, **kwargs)
