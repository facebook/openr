#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


from builtins import object

from thrift.protocol.TCompactProtocol import TCompactProtocolFactory


class Consts(object):
    TIMEOUT_MS = 5000
    CONST_TTL_INF = -2 ** 31
    IP_TOS = 192
    ADJ_DB_MARKER = "adj:"
    PREFIX_DB_MARKER = "prefix:"
    ALL_DB_MARKER = ""

    SEED_PREFIX_ALLOC_PARAM_KEY = "e2e-network-prefix"
    STATIC_PREFIX_ALLOC_PARAM_KEY = "e2e-network-allocations"

    CTRL_PORT = 2018
    KVSTORE_REP_PORT = 60002
    KVSTORE_PUB_PORT = 60001
    MONITOR_PUB_PORT = 60007
    MONITOR_REP_PORT = 60008
    FIB_AGENT_PORT = 60100
    CONFIG_STORE_URL = "ipc:///tmp/openr_config_store_cmd"
    FORCE_CRASH_SERVER_URL = "ipc:///tmp/force_crash_server"

    TOPOLOGY_OUTPUT_FILE = "/tmp/openr-topology.png"

    PREFIX_ALLOC_KEY = "prefix-allocator-config"
    LINK_MONITOR_KEY = "link-monitor-config"
    PREFIX_MGR_KEY = "prefix-manager-config"

    # Default serializer/deserializer for communication with OpenR
    PROTO_FACTORY = TCompactProtocolFactory

    OPENR_CONFIG_FILE = "/etc/sysconfig/openr"
