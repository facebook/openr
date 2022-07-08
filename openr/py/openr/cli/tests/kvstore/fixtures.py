#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict


from enum import Enum

from openr.KvStore import ttypes as kvstore_types
from openr.thrift.OpenrConfig.types import AreaConfig, OpenrConfig


class AreaId(Enum):
    AREA1 = "area1"
    AREA2 = "area2"
    AREA3 = "area3"


MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS = OpenrConfig(
    areas=[
        AreaConfig(area_id=AreaId.AREA1.value),
        AreaConfig(area_id=AreaId.AREA2.value),
        AreaConfig(area_id=AreaId.AREA3.value),
    ]
)

MOCKED_KVSTORE_PEERS_TWO_PEERS = {
    "node2": kvstore_types.PeerSpec(
        peerAddr="fe80::b81a:ceff:fe2b:d473%if_1_2_1",
        ctrlPort=2018,
        state=kvstore_types.KvStorePeerState.INITIALIZED,
    ),
    "node15": kvstore_types.PeerSpec(
        peerAddr="fe80::a433:47ff:feaa:fd8d%if_1_15_1",
        ctrlPort=2018,
        state=kvstore_types.KvStorePeerState.INITIALIZED,
    ),
}

MOCKED_KVSTORE_PEERS_ONE_PEER = {
    "node5": kvstore_types.PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_5_1",
        ctrlPort=2018,
        state=kvstore_types.KvStorePeerState.INITIALIZED,
    )
}
