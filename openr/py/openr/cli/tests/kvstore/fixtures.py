#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict


from enum import Enum
from typing import Dict

from openr.thrift.KvStore.thrift_types import (
    InitializationEvent,
    KvStorePeerState,
    PeerSpec,
    Value,
)
from openr.thrift.OpenrConfig.thrift_types import AreaConfig, KvstoreConfig, OpenrConfig


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
    VAL1 = Value(ttl=3600000, ttlVersion=1)
    VAL2 = Value(ttl=3599990, ttlVersion=2)
    VAL3 = Value(ttl=3599989, ttlVersion=3)
    VAL4 = Value(ttl=3599982, ttlVersion=4)
    VAL5 = Value(ttl=3599810, ttlVersion=5)
    VAL6 = Value(ttl=9999999, ttlVersion=6)


class MockedInvalidKeyVals(Enum):
    VAL1 = Value(
        version=1,
        originatorId=NodeNames.MOCKED_NODE1.value,
        value=None,
        ttl=1700000,
        ttlVersion=1,
        hash=12345,
    )
    VAL2 = Value(
        version=2,
        originatorId=NodeNames.MOCKED_NODE2.value,
        value=None,
        ttl=1699990,
        ttlVersion=2,
        hash=-23456,
    )
    VAL3 = Value(
        version=2,
        originatorId=NodeNames.MOCKED_NODE1.value,
        value=None,
        ttl=4900000,
        ttlVersion=6,
        hash=34567,
    )


MOCKED_THRIFT_CONFIG_MULTIPLE_AREAS = OpenrConfig(
    node_name=NodeNames.MOCKED_NODE1.value,
    areas=[
        AreaConfig(area_id=AreaId.AREA1.value),
        AreaConfig(area_id=AreaId.AREA2.value),
        AreaConfig(area_id=AreaId.AREA3.value),
    ],
    kvstore_config=KvstoreConfig(key_ttl_ms=3600000),
)

MOCKED_THRIFT_CONFIG_ONE_AREA = OpenrConfig(
    node_name=NodeNames.MOCKED_NODE2.value,
    areas=[AreaConfig(area_id=AreaId.AREA1.value)],
    kvstore_config=KvstoreConfig(key_ttl_ms=3600000),
)

MOCKED_THRIFT_CONFIG_ONE_AREA_HIGH_TTL = OpenrConfig(
    node_name=NodeNames.MOCKED_NODE2.value,
    areas=[AreaConfig(area_id=AreaId.AREA1.value)],
    kvstore_config=KvstoreConfig(key_ttl_ms=10000000),
)

MOCKED_KVSTORE_PEERS_TWO_PEERS = {
    "node2": PeerSpec(
        peerAddr="fe80::b81a:ceff:fe2b:d473%if_1_2_1",
        ctrlPort=2018,
        state=KvStorePeerState.INITIALIZED,
    ),
    "node15": PeerSpec(
        peerAddr="fe80::a433:47ff:feaa:fd8d%if_1_15_1",
        ctrlPort=2018,
        state=KvStorePeerState.INITIALIZED,
    ),
}

MOCKED_KVSTORE_PEERS_ONE_PEER = {
    "node5": PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_5_1",
        ctrlPort=2018,
        state=KvStorePeerState.INITIALIZED,
    )
}

MOCKED_KVSTORE_PEERS_DIFF_STATES = {
    "node6": PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_6_1",
        ctrlPort=1000,
        state=KvStorePeerState.INITIALIZED,
    ),
    "node12": PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_12_1",
        ctrlPort=1000,
        state=KvStorePeerState.SYNCING,
    ),
    "node18": PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_18_1",
        ctrlPort=1000,
        state=KvStorePeerState.IDLE,
    ),
}

MOCKED_KVSTORE_PEERS_ONE_FAIL = {
    "node20": PeerSpec(
        peerAddr="fe80::d81a:feff:fc4b:d213%if_1_20_1",
        ctrlPort=1000,
        state=KvStorePeerState.IDLE,
    ),
}

MOCKED_INIT_EVENTS_PASS: dict[InitializationEvent, int] = {
    InitializationEvent.KVSTORE_SYNCED: 9204,
}
