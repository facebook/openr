#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import copy
import re
import ssl
from typing import Any, Callable, Tuple

import bunch
import click
from openr.cli.utils.default_option_overrides import getDefaultOption
from openr.Platform import ttypes as platform_types
from openr.utils.consts import Consts


SSL_CERT_REQS = {
    "none": ssl.CERT_NONE,
    "optional": ssl.CERT_OPTIONAL,
    "required": ssl.CERT_REQUIRED,
}

OPTIONS = bunch.Bunch(
    {
        "acceptable_peer_name": "",
        "area": "",
        "ca_file": "",
        "cert_file": "",
        "cert_reqs": ssl.CERT_NONE,
        "client_id": platform_types.FibClient.OPENR,
        "fib_agent_port": Consts.FIB_AGENT_PORT,
        "host": "localhost",
        "key_file": "",
        "openr_ctrl_port": Consts.CTRL_PORT,
        "proto_factory": Consts.PROTO_FACTORY,
        "ssl": False,
        "timeout": Consts.TIMEOUT_MS,
    }
)


def getDefaultOptions(host: str, timeout_ms: int = Consts.TIMEOUT_MS) -> bunch.Bunch:
    """
    get all default options for a given host
    """

    options = copy.deepcopy(OPTIONS)
    # pyre-fixme[16]: `Bunch` has no attribute `host`.
    options.host = host
    # pyre-fixme[16]: `Bunch` has no attribute `timeout`.
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


def getNameFromOpts(opts: Tuple[Any, ...]) -> str:
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
