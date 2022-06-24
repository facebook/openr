#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-ignore-all-errors

from openr.KvStore import ttypes as openr_kvstore_types
from openr.Network.ttypes import BinaryAddress
from openr.Types.ttypes import SparkNeighbor
from openr.utils.consts import Consts

MOCKED_SPARK_NEIGHBORS = [
    SparkNeighbor(
        nodeName="node2",
        state="NEGOTIATE",
        event="HELLO_INFO_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=Consts.DEFAULT_AREA_ID,
        remoteIfName="if_2_1_1",
        localIfName="if_1_2_1",
        rttUs=1000,
    ),
    SparkNeighbor(
        nodeName="node5",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=Consts.DEFAULT_AREA_ID,
        remoteIfName="if_5_1_1",
        localIfName="if_1_5_1",
        rttUs=1000,
    ),
]

SPARK_NEIGHBORS_OUTPUT = """\

 Neighbor    State        Latest Event     Local Intf    Remote Intf      Area    Rtt(us)
----------  -----------  ---------------  ------------  -------------  ------  ---------
node2       NEGOTIATE    HELLO_INFO_RCVD  if_1_2_1      if_2_1_1            0       1000
node5       ESTABLISHED  HANDSHAKE_RCVD   if_1_5_1      if_5_1_1            0       1000
"""

# Keeping for reference
SPARK_NEIGHBORS_OUTPUT_JSON = """\
[
  {
    "area": "0",
    "event": "HELLO_INFO_RCVD",
    "lastHandshakeMsgSentTimeDelta": 0,
    "lastHeartbeatMsgSentTimeDelta": 0,
    "lastHelloMsgSentTimeDelta": 0,
    "localIfName": "if_1_2_1",
    "nodeName": "node2",
    "openrCtrlThriftPort": 2018,
    "remoteIfName": "if_2_1_1",
    "rttUs": 1000,
    "state": "NEGOTIATE",
    "transportAddressV4": {
      "addr": "0.0.0.0",
      "ifName": null
    },
    "transportAddressV6": {
      "addr": "::",
      "ifName": null
    }
  },
  {
    "area": "0",
    "event": "HANDSHAKE_RCVD",
    "lastHandshakeMsgSentTimeDelta": 0,
    "lastHeartbeatMsgSentTimeDelta": 0,
    "lastHelloMsgSentTimeDelta": 0,
    "localIfName": "if_1_5_1",
    "nodeName": "node5",
    "openrCtrlThriftPort": 2018,
    "remoteIfName": "if_5_1_1",
    "rttUs": 1000,
    "state": "ESTABLISHED",
    "transportAddressV4": {
      "addr": "0.0.0.0",
      "ifName": null
    },
    "transportAddressV6": {
      "addr": "::",
      "ifName": null
    }
  }
]
"""

MOCKED_SPARK_NEIGHBORS_NO_ESTAB = [
    SparkNeighbor(
        nodeName="node2",
        state="NEGOTIATE",
        event="HELLO_INFO_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=Consts.DEFAULT_AREA_ID,
        remoteIfName="if_2_1_1",
        localIfName="if_1_2_1",
        rttUs=1000,
    ),
    SparkNeighbor(
        nodeName="node5",
        state="WARM",
        event="NEGOTIATION_FAILURE",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=Consts.DEFAULT_AREA_ID,
        remoteIfName="if_5_1_1",
        localIfName="if_1_5_1",
        rttUs=1000,
    ),
]

MOCKED_SPARK_NEIGHBORS_ALL_ESTAB = [
    SparkNeighbor(
        nodeName="node2",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=Consts.DEFAULT_AREA_ID,
        remoteIfName="if_2_1_1",
        localIfName="if_1_2_1",
        rttUs=1000,
    ),
    SparkNeighbor(
        nodeName="node5",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=Consts.DEFAULT_AREA_ID,
        remoteIfName="if_5_1_1",
        localIfName="if_1_5_1",
        rttUs=1000,
    ),
]

MOCKED_INIT_EVENTS = {
    openr_kvstore_types.InitializationEvent.INITIALIZING: 1,
    openr_kvstore_types.InitializationEvent.AGENT_CONFIGURED: 4,
    openr_kvstore_types.InitializationEvent.LINK_DISCOVERED: 2401,
    openr_kvstore_types.InitializationEvent.NEIGHBOR_DISCOVERED: 2400,
    openr_kvstore_types.InitializationEvent.KVSTORE_SYNCED: 2403,
    openr_kvstore_types.InitializationEvent.RIB_COMPUTED: 9204,
    openr_kvstore_types.InitializationEvent.FIB_SYNCED: 9205,
    openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED: 9206,
}

MOCKED_INIT_EVEVENTS_TIMEOUT = {
    openr_kvstore_types.InitializationEvent.INITIALIZING: 1,
    openr_kvstore_types.InitializationEvent.AGENT_CONFIGURED: 4,
    openr_kvstore_types.InitializationEvent.LINK_DISCOVERED: 2401,
    openr_kvstore_types.InitializationEvent.NEIGHBOR_DISCOVERED: 61040,
}

MOCKED_INIT_EVEVENTS_WARNING = {
    openr_kvstore_types.InitializationEvent.INITIALIZING: 1,
    openr_kvstore_types.InitializationEvent.AGENT_CONFIGURED: 4,
    openr_kvstore_types.InitializationEvent.LINK_DISCOVERED: 2401,
    openr_kvstore_types.InitializationEvent.NEIGHBOR_DISCOVERED: 38910,
}

MOCKED_INIT_EVEVENTS_NO_PUBLISH = {
    openr_kvstore_types.InitializationEvent.INITIALIZING: 1,
    openr_kvstore_types.InitializationEvent.AGENT_CONFIGURED: 4,
    openr_kvstore_types.InitializationEvent.LINK_DISCOVERED: 2401,
}
