#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-ignore-all-errors

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
