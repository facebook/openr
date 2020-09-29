#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import re
from builtins import object

from thrift.protocol.TCompactProtocol import TCompactProtocolFactory


class Consts(object):
    TIMEOUT_MS = 10000  # 10 seconds
    CONST_TTL_INF = -(2 ** 31)
    IP_TOS = 192
    ADJ_DB_MARKER = "adj:"
    PREFIX_DB_MARKER = "prefix:"
    ALL_DB_MARKER = ""

    SEED_PREFIX_ALLOC_PARAM_KEY = "e2e-network-prefix"
    STATIC_PREFIX_ALLOC_PARAM_KEY = "e2e-network-allocations"

    CTRL_PORT = 2018
    FIB_AGENT_PORT = 60100

    TOPOLOGY_OUTPUT_FILE = "/tmp/openr-topology.png"

    PREFIX_ALLOC_KEY = "prefix-allocator-config"
    LINK_MONITOR_KEY = "link-monitor-config"
    PREFIX_MGR_KEY = "prefix-manager-config"

    # Default serializer/deserializer for communication with OpenR
    PROTO_FACTORY = TCompactProtocolFactory

    OPENR_CONFIG_FILE = "/etc/sysconfig/openr"

    # per prefix key regex for the following formats
    # prefix:e00.0002.node2:area1:[192.168.0.2/32]
    # prefix:e00.0002.node2:area2:[da00:cafe:babe:51:61ee::/80]
    PER_PREFIX_KEY_REGEX = (
        re.escape(PREFIX_DB_MARKER)
        + r"(?P<node>[A-Za-z0-9_-].*):"
        + r"(?P<area>[A-Za-z0-9].*):"
        + r"\[(?P<ipaddr>[a-fA-F0-9\.\:].*)/"
        + r"(?P<plen>[0-9]{1,3})\]"
    )
