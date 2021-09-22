#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
from typing import Any, Dict, Optional

from openr.Network import ttypes as network_types
from openr.utils.consts import Consts
from openr.utils.ipnetwork import sprint_prefix
from thrift.util import Serializer


def object_to_dict(data: Any, overrides: Optional[Dict] = None) -> Any:
    """
    Recursively convert any python object to dict such that it is json
    serializable. Provides to_dict overrides for any specific class type
    """

    # Handling none objects
    if data is None:
        return None

    # Sequence type
    if type(data) in [list, tuple, range]:
        return [object_to_dict(x, overrides) for x in data]

    # Set type
    if type(data) in [set, frozenset]:
        return sorted([object_to_dict(x, overrides) for x in data])

    # Map type
    if isinstance(data, dict):
        return {
            object_to_dict(x, overrides): object_to_dict(y, overrides)
            for x, y in data.items()
        }

    # Bytes type
    if isinstance(data, bytes):
        return data.decode("utf-8")

    # Primitive types (string & numbers)
    if type(data) in [float, int, str]:
        return data

    # Specific overrides
    if overrides:
        for class_type, override_fn in overrides.items():
            if isinstance(data, class_type):
                return override_fn(data)

    # data represents some class
    return {x: object_to_dict(y, overrides) for x, y in data.__dict__.items()}


def serialize_json(struct: Any) -> str:
    """Serialize any thrift Struct into JSON"""

    overrides = {
        # Convert IpPrefix to human readable format
        network_types.IpPrefix: sprint_prefix,
    }
    return json.dumps(object_to_dict(struct, overrides), indent=2, sort_keys=True)


def serialize_thrift_object(thrift_obj, proto_factory=Consts.PROTO_FACTORY):
    """Serialize thrift data to binary blob

    :param thrift_obj: the thrift object
    :param proto_factory: protocol factory, set default as Compact Protocol

    :return: string the serialized thrift payload
    """

    return Serializer.serialize(proto_factory(), thrift_obj)


def deserialize_thrift_object(
    raw_data, thrift_type, proto_factory=Consts.PROTO_FACTORY
):
    """Deserialize thrift data from binary blob

    :param raw_data string: the serialized thrift payload
    :param thrift_type: the thrift type
    :param proto_factory: protocol factory, set default as Compact Protocol

    :return: instance of thrift_type
    """

    resp = thrift_type()
    Serializer.deserialize(proto_factory(), raw_data, resp)
    return resp
