#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import copy
import re
import ssl
from collections.abc import Callable
from typing import Any, Tuple

import bunch
import click
from openr.py.openr.cli.utils.default_option_overrides import getDefaultOption
from openr.py.openr.utils.consts import Consts
from openr.thrift.Platform import thrift_types as platform_types


SSL_CERT_REQS = {
    "none": ssl.CERT_NONE,
    "optional": ssl.CERT_OPTIONAL,
    "required": ssl.CERT_REQUIRED,
}

OPTIONS = bunch.Bunch(
    {
        "acceptable_peer_name": "",
        "area": "",
        "ssl": False,
        "ca_file": "",
        "cert_file": "",
        "key_file": "",
        "cert_reqs": ssl.CERT_NONE,
        "client_id": platform_types.FibClient.OPENR,
        "fib_agent_port": Consts.FIB_AGENT_PORT,
        "host": "localhost",
        "openr_ctrl_port": Consts.CTRL_PORT,
        "proto_factory": Consts.PROTO_FACTORY,
        "timeout": Consts.TIMEOUT_MS,
    }
)


def getDefaultOptions(host: str, timeout_ms: int = Consts.TIMEOUT_MS) -> bunch.Bunch:
    """
    get all default options for a given host
    """

    options = copy.deepcopy(OPTIONS)
    options.host = host
    options.timeout = timeout_ms
    for k in options:
        options[k] = getDefaultOption(options, k)
    return options


def nameFromOpt(opt: str) -> str:
    return re.sub("^-+", "", opt.split("/")[0]).replace("-", "_")


def str2cert(ctx, param, value) -> None:
    name = getNameFromOpts(param.opts)
    OPTIONS[name] = (
        SSL_CERT_REQS[value] if value is not None else getDefaultOption(OPTIONS, name)
    )


def getNameFromOpts(opts: tuple[Any, ...]) -> str:
    names = [nameFromOpt(n) for n in opts if nameFromOpt(n) in OPTIONS]
    assert len(names) == 1, "Exaclty one parameter must correspond to an option"
    return names[0]


def set_option(ctx, param, value) -> None:
    name = getNameFromOpts(param.opts)
    OPTIONS[name] = value if value is not None else getDefaultOption(OPTIONS, name)


def breeze_option(*args: Any, **kwargs: Any) -> Callable[[Any], Any]:
    assert "default" not in kwargs and getNameFromOpts(args) in OPTIONS
    kwargs["default"] = None
    if "callback" not in kwargs:
        kwargs["callback"] = set_option
    return click.option(*args, **kwargs)
