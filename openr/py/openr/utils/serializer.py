#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
from typing import Any

from openr.py.openr.utils.ipnetwork import sprint_addr, sprint_prefix
from openr.thrift.Network.thrift_types import BinaryAddress, IpPrefix
from thrift.python import types as thrift_python_types


TO_DICT_OVERRIDES = {
    # Convert IpPrefix to human readable format
    IpPrefix: sprint_prefix,
    # Convert BinaryAddress to human readable format
    BinaryAddress: lambda x: {
        "addr": sprint_addr(x.addr),
        "ifName": x.ifName,
    },
}


def object_to_dict(data: Any, overrides: dict | None = TO_DICT_OVERRIDES) -> Any:
    """
    Recursively convert any python object to dict such that it is json
    serializable. Provides to_dict overrides for any specific class type
    """

    # Handling none objects
    if data is None:
        return None

    # Sequence type
    if type(data) in [list, tuple, range, thrift_python_types.List]:
        return [object_to_dict(x, overrides) for x in data]

    # Set type
    if type(data) in [set, frozenset, thrift_python_types.Set]:
        return sorted([object_to_dict(x, overrides) for x in data])

    # Map type
    if type(data) in [dict, thrift_python_types.Map]:
        return {
            object_to_dict(x, overrides): object_to_dict(y, overrides)
            for x, y in data.items()
        }

    # Bytes type
    if isinstance(data, bytes):
        return data.decode("utf-8")

    # Primitive types (string & numbers)
    if type(data) in [float, int, str, bool]:
        return data

    # Specific overrides
    if overrides:
        for class_type, override_fn in overrides.items():
            if isinstance(data, class_type):
                return override_fn(data)

    # thrift-python enums
    if isinstance(data, thrift_python_types.Enum):
        return data.value

    # thrift-python structs
    if isinstance(data, thrift_python_types.Struct):
        return {x: object_to_dict(y, overrides) for x, y in data}

    # data represents some class
    return {x: object_to_dict(y, overrides) for x, y in data.__dict__.items()}


def serialize_json(struct: Any) -> str:
    """Serialize any thrift Struct into JSON"""

    return json.dumps(object_to_dict(struct), indent=2, sort_keys=True)
