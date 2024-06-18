#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-ignore-all-errors

from enum import Enum

from openr.py.openr.utils.consts import Consts

from openr.thrift.KvStore import thrift_types as openr_kvstore_types
from openr.thrift.Network.thrift_types import BinaryAddress
from openr.thrift.OpenrConfig.thrift_types import AreaConfig, OpenrConfig
from openr.thrift.Types.thrift_types import SparkNeighbor

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


class AreaId(Enum):
    ACCEPT_ALL = "accept-all-areaId"
    ACCEPT_ALIENS = "accept-aliens-areaId"
    ACCEPT_NODES = "accept-nodes-areaId"
    ACCEPT_NONE = "accept-none-area-id"


MOCKED_AREA_ACCEPT_ALL = AreaConfig(
    area_id=AreaId.ACCEPT_ALL.value, neighbor_regexes=[".*"]
)

MOCKED_AREA_ACCEPT_NONE = AreaConfig(
    area_id=AreaId.ACCEPT_NONE.value, neighbor_regexes=[]
)

MOCKED_AREA_ACCEPT_ALIENS = AreaConfig(
    area_id=AreaId.ACCEPT_ALIENS.value, neighbor_regexes=["alien.*"]
)

MOCKED_AREA_ACCEPT_NODES = AreaConfig(
    area_id=AreaId.ACCEPT_NODES.value, neighbor_regexes=["node.*"]
)

MOCKED_CONFIG_ACCEPT_NONE = OpenrConfig(areas=[MOCKED_AREA_ACCEPT_NONE])

MOCKED_CONFIG_ACCEPT_ALL = OpenrConfig(areas=[MOCKED_AREA_ACCEPT_ALL])

MOCKED_CONFIG_ACCEPT_ALIENS_NODES = OpenrConfig(
    areas=[MOCKED_AREA_ACCEPT_ALIENS, MOCKED_AREA_ACCEPT_NODES]
)

MOCKED_CONFIG_DEFAULT = OpenrConfig(
    areas=[AreaConfig(area_id=Consts.DEFAULT_AREA_ID, neighbor_regexes=[".*"])]
)


MOCKED_SPARK_NEIGHBORS_ALIENS = [
    SparkNeighbor(
        nodeName="alien2",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=AreaId.ACCEPT_NODES.value,
        remoteIfName="if_2_1_1",
        localIfName="if_1_2_1",
        rttUs=1000,
    ),
    SparkNeighbor(
        nodeName="alien5",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=AreaId.ACCEPT_ALIENS.value,
        remoteIfName="if_5_1_1",
        localIfName="if_1_5_1",
        rttUs=1000,
    ),
]

MOCKED_SPARK_NEIGHBORS_DIFF_IDS_ACCEPT_ALL = [
    SparkNeighbor(
        nodeName="alien2",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=AreaId.ACCEPT_ALL.value,
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
        area=AreaId.ACCEPT_ALL.value,
        remoteIfName="if_5_1_1",
        localIfName="if_1_5_1",
        rttUs=1000,
    ),
]

MOCKED_SPARK_NEIGHBORS_DIFF_IDS = [
    SparkNeighbor(
        nodeName="node2",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=AreaId.ACCEPT_NODES.value,
        remoteIfName="if_2_1_1",
        localIfName="if_1_2_1",
        rttUs=1000,
    ),
    SparkNeighbor(
        nodeName="alien5",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=AreaId.ACCEPT_ALIENS.value,
        remoteIfName="if_5_1_1",
        localIfName="if_1_5_1",
        rttUs=1000,
    ),
]

MOCKED_SPARK_NEIGHBORS_NO_ACCEPT = [
    SparkNeighbor(
        nodeName="node2",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=AreaId.ACCEPT_NONE.value,
        remoteIfName="if_2_1_1",
        localIfName="if_1_2_1",
        rttUs=1000,
    ),
    SparkNeighbor(
        nodeName="alien5",
        state="ESTABLISHED",
        event="HANDSHAKE_RCVD",
        transportAddressV6=BinaryAddress(
            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        ),
        transportAddressV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
        openrCtrlThriftPort=Consts.CTRL_PORT,
        area=AreaId.ACCEPT_NONE.value,
        remoteIfName="if_5_1_1",
        localIfName="if_1_5_1",
        rttUs=1000,
    ),
]
