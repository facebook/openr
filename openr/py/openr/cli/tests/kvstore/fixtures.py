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


class NodeNames(Enum):
    MOCKED_NODE1 = "mocked_node1"
    MOCKED_NODE2 = "mocked_node2"


class MockedKeys(Enum):
    ADJ1 = f"adj:{NodeNames.MOCKED_NODE1.value}"
    PREF1 = f"prefix:{NodeNames.MOCKED_NODE1.value}:[10.188.128.0/28]"
    PREF2 = f"prefix:{NodeNames.MOCKED_NODE1.value}:[10.188.128.16/28]"
    PREF3 = f"prefix:{NodeNames.MOCKED_NODE1.value}:[10.188.128.1/32]"
    PREF4 = f"prefix:{NodeNames.MOCKED_NODE1.value}:[10.163.56.0/26]"


class MockedValidKeyVals(Enum):
    VAL1 = kvstore_types.Value(ttl=3600000, ttlVersion=1)
    VAL2 = kvstore_types.Value(ttl=3599990, ttlVersion=1)
    VAL3 = kvstore_types.Value(ttl=3599989, ttlVersion=1)
    VAL4 = kvstore_types.Value(ttl=3599982, ttlVersion=1)
    VAL5 = kvstore_types.Value(ttl=3599810, ttlVersion=1)


MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS = OpenrConfig(
    node_name=NodeNames.MOCKED_NODE1.value,
    areas=[
        AreaConfig(area_id=AreaId.AREA1.value),
        AreaConfig(area_id=AreaId.AREA2.value),
        AreaConfig(area_id=AreaId.AREA3.value),
    ],
)

MOCKED_THRIFT_CONFIG_ONE_AREA = OpenrConfig(
    node_name=NodeNames.MOCKED_NODE2.value,
    areas=[AreaConfig(area_id=AreaId.AREA1.value)],
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

MOCKED_KVSTORE_PEERS_DIFF_STATES = {
    "node6": kvstore_types.PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_6_1",
        ctrlPort=1000,
        state=kvstore_types.KvStorePeerState.INITIALIZED,
    ),
    "node12": kvstore_types.PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_12_1",
        ctrlPort=1000,
        state=kvstore_types.KvStorePeerState.SYNCING,
    ),
    "node18": kvstore_types.PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_18_1",
        ctrlPort=1000,
        state=kvstore_types.KvStorePeerState.IDLE,
    ),
}

MOCKED_KVSTORE_PEERS_ONE_FAIL = {
    "node20": kvstore_types.PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_20_1",
        ctrlPort=1000,
        state=kvstore_types.KvStorePeerState.IDLE,
    ),
}
