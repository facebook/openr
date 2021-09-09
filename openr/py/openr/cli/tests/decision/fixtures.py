#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-ignore-all-errors

from openr.Network.ttypes import (
    BinaryAddress,
    IpPrefix,
)
from openr.OpenrCtrl.ttypes import (
    NodeAndArea,
    ReceivedRoute,
    ReceivedRouteDetail,
)
from openr.Types.ttypes import (
    Adjacency,
    AdjacencyDatabase,
    KvStoreAreaSummary,
    PerfEvents,
    PerfEvent,
    PrefixEntry,
    PrefixMetrics,
    Publication,
    Value,
)


AREA_SUMMARIES = (
    KvStoreAreaSummary(
        area="area2",
        peersMap=None,
        keyValsBytes=70,
        keyValsCount=70,
    ),
    KvStoreAreaSummary(
        area="area1",
        peersMap=None,
        keyValsBytes=69,
        keyValsCount=69,
    ),
)


DECISION_ADJ_DBS_OK = [
    AdjacencyDatabase(
        thisNodeName="openr-right",
        isOverloaded=False,
        adjacencies=[
            Adjacency(
                otherNodeName="openr-center",
                ifName="right0",
                nextHopV6=BinaryAddress(
                    addr=b"\xfe\x80\x00\x00\x00\x00\x00\x00 \xa2\x01\xff\xfe\xf4Y\xbe"
                ),
                nextHopV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
                metric=1,
                adjLabel=0,
                isOverloaded=False,
                rtt=0,
                timestamp=1631215060,
                weight=1,
                otherIfName="area2",
            )
        ],
        nodeLabel=0,
        perfEvents=PerfEvents(
            events=[
                PerfEvent(
                    nodeName="openr-right",
                    eventDescr="ADJ_DB_UPDATED",
                    unixTs=1631213280260,
                )
            ]
        ),
        area="area2",
    ),
    AdjacencyDatabase(
        thisNodeName="openr-center",
        isOverloaded=False,
        adjacencies=[
            Adjacency(
                otherNodeName="openr-right",
                ifName="area2",
                nextHopV6=BinaryAddress(
                    addr=b"\xfe\x80\x00\x00\x00\x00\x00\x00\xe0\x03\xc6\xff\xfe`\x10("
                ),
                nextHopV4=BinaryAddress(addr=b"\x00\x00\x00\x00"),
                metric=1,
                adjLabel=0,
                isOverloaded=False,
                rtt=0,
                timestamp=1631215060,
                weight=1,
                otherIfName="right0",
            )
        ],
        nodeLabel=0,
        perfEvents=PerfEvents(
            events=[
                PerfEvent(
                    nodeName="openr-center",
                    eventDescr="ADJ_DB_UPDATED",
                    unixTs=1631213278744,
                )
            ]
        ),
        area="area2",
    ),
]

RECEIVED_ROUTES_DB_OK = [
    ReceivedRouteDetail(
        prefix=IpPrefix(
            prefixAddress=BinaryAddress(
                addr=b"\xfd\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            ),
            prefixLength=64,
        ),
        bestKey=NodeAndArea(node="openr-right", area="area2"),
        bestKeys=[NodeAndArea(node="openr-right", area="area2")],
        routes=[
            ReceivedRoute(
                key=NodeAndArea(node="openr-right", area="area2"),
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\xfd\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        ),
                        prefixLength=64,
                    ),
                    type=1,
                    forwardingType=0,
                    forwardingAlgorithm=0,
                    metrics=PrefixMetrics(
                        version=1,
                        path_preference=1000,
                        source_preference=200,
                        distance=0,
                    ),
                    tags={"INTERFACE_SUBNET", "openr-right:lo"},
                    area_stack=[],
                ),
            )
        ],
    ),
    ReceivedRouteDetail(
        prefix=IpPrefix(
            prefixAddress=BinaryAddress(
                addr=b"\xfd\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            ),
            prefixLength=64,
        ),
        bestKey=NodeAndArea(node="openr-right", area="area2"),
        bestKeys=[NodeAndArea(node="openr-right", area="area2")],
        routes=[
            ReceivedRoute(
                key=NodeAndArea(node="openr-right", area="area2"),
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\xfd\x00\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        ),
                        prefixLength=64,
                    ),
                    type=1,
                    forwardingType=0,
                    forwardingAlgorithm=0,
                    metrics=PrefixMetrics(
                        version=1,
                        path_preference=1000,
                        source_preference=200,
                        distance=0,
                    ),
                    tags={"INTERFACE_SUBNET", "openr-right:lo"},
                    area_stack=[],
                ),
            )
        ],
    ),
    ReceivedRouteDetail(
        prefix=IpPrefix(
            prefixAddress=BinaryAddress(
                addr=b"\xfd\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            ),
            prefixLength=64,
        ),
        bestKey=NodeAndArea(node="openr-center", area="area2"),
        bestKeys=[NodeAndArea(node="openr-center", area="area2")],
        routes=[
            ReceivedRoute(
                key=NodeAndArea(node="openr-center", area="area2"),
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\xfd\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        ),
                        prefixLength=64,
                    ),
                    type=1,
                    forwardingType=0,
                    forwardingAlgorithm=0,
                    metrics=PrefixMetrics(
                        version=1,
                        path_preference=1000,
                        source_preference=200,
                        distance=0,
                    ),
                    tags={"INTERFACE_SUBNET", "openr-center:lo"},
                    area_stack=[],
                ),
            )
        ],
    ),
    ReceivedRouteDetail(
        prefix=IpPrefix(
            prefixAddress=BinaryAddress(
                addr=b"\xfd\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            ),
            prefixLength=64,
        ),
        bestKey=NodeAndArea(node="openr-center", area="area2"),
        bestKeys=[NodeAndArea(node="openr-center", area="area2")],
        routes=[
            ReceivedRoute(
                key=NodeAndArea(node="openr-center", area="area2"),
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\xfd\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        ),
                        prefixLength=64,
                    ),
                    type=1,
                    forwardingType=0,
                    forwardingAlgorithm=0,
                    metrics=PrefixMetrics(
                        version=1,
                        path_preference=1000,
                        source_preference=200,
                        distance=0,
                    ),
                    tags={"INTERFACE_SUBNET", "openr-center:lo"},
                    area_stack=[],
                ),
            )
        ],
    ),
]

KVSTORE_KEYVALS_OK = Publication(
    keyVals={
        "adj:openr-center": Value(
            version=1,
            originatorId="openr-center",
            value=(
                b"\x18\x0copenr-center\x12\x19\x1c\x18\x0bopenr-right\x18\x05area2\x1c\x18\x10"
                b"\xfe\x80\x00\x00\x00\x00\x00\x00\xe0\x03\xc6\xff\xfe`\x10(\x00,\x18\x04"
                b"\x00\x00\x00\x00\x00\x05\x08\x02%\x00\x12\x15\x00\x16\xa8\xf7"
                b"\xd2\x93\x0c\x16\x02\x18\x06right0\x00\x15\x00\x1c\x19\x1c\x18\x0copenr-c"
                b"enter\x18\x0eADJ_DB_UPDATED\x16\xb6\xbc\xf4\xbf\xf9^\x00\x00\x18\x05area"
                b"2\x00"
            ),
            ttl=6283,
            ttlVersion=28,
            hash=-7578663388807274241,
        ),
        "adj:openr-right": Value(
            version=2,
            originatorId="openr-right",
            value=(
                b"\x18\x0bopenr-right\x12\x19\x1c\x18\x0copenr-center\x18\x06right0\x1c\x18"
                b"\x10\xfe\x80\x00\x00\x00\x00\x00\x00 \xa2\x01\xff\xfe\xf4Y\xbe\x00,\x18"
                b"\x04\x00\x00\x00\x00\x00\x05\x08\x02%\x00\x12\x15\x00\x16\xa8"
                b"\xf7\xd2\x93\x0c\x16\x02\x18\x05area2\x00\x15\x00\x1c\x19\x1c\x18\x0bopenr-r"
                b"ight\x18\x0eADJ_DB_UPDATED\x16\xe2\xa4\xf4\xbf\xf9^\x00\x00\x18\x05area2"
                b"\x00"
            ),
            ttl=6494,
            ttlVersion=29,
            hash=8748446911101193323,
        ),
        "prefix:openr-center:area2:[fd00:4::/64]": Value(
            version=1,
            originatorId="openr-center",
            value=(
                b"\x18\x0copenr-center)\x1c\x1c\x1c\x18\x10\xfd\x00\x00\x04\x00\x00\x00\x00"
                b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x14\x80\x01\x00\x15\x02%\x005\x00<"
                b"\x15\x02\x15\xd0\x0f\x15\x90\x03\x15\x00\x00\x1a(\x10INTERFACE_SUBNET\x0fo"
                b'penr-center:lo\x19\x08\x00"(\x05area2\x00'
            ),
            ttl=6733,
            ttlVersion=22,
            hash=-1767333853492350191,
        ),
        "prefix:openr-center:area2:[fd00:5::/64]": Value(
            version=1,
            originatorId="openr-center",
            value=(
                b"\x18\x0copenr-center)\x1c\x1c\x1c\x18\x10\xfd\x00\x00\x05\x00\x00\x00\x00"
                b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x14\x80\x01\x00\x15\x02%\x005\x00<"
                b"\x15\x02\x15\xd0\x0f\x15\x90\x03\x15\x00\x00\x1a(\x10INTERFACE_SUBNET\x0fo"
                b'penr-center:lo\x19\x08\x00"(\x05area2\x00'
            ),
            ttl=6733,
            ttlVersion=22,
            hash=7762225429084363332,
        ),
        "prefix:openr-right:area2:[fd00:6::/64]": Value(
            version=1,
            originatorId="openr-right",
            value=(
                b"\x18\x0bopenr-right)\x1c\x1c\x1c\x18\x10\xfd\x00\x00\x06\x00\x00\x00\x00\x00"
                b"\x00\x00\x00\x00\x00\x00\x00\x00\x14\x80\x01\x00\x15\x02%\x005\x00<\x15"
                b"\x02\x15\xd0\x0f\x15\x90\x03\x15\x00\x00\x1a(\x10INTERFACE_SUBNET\x0eopenr-"
                b'right:lo\x19\x08\x00"(\x05area2\x00'
            ),
            ttl=5793,
            ttlVersion=24,
            hash=-8864065810907974827,
        ),
        "prefix:openr-right:area2:[fd00:7::/64]": Value(
            version=1,
            originatorId="openr-right",
            value=(
                b"\x18\x0bopenr-right)\x1c\x1c\x1c\x18\x10\xfd\x00\x00\x07\x00\x00\x00\x00\x00"
                b"\x00\x00\x00\x00\x00\x00\x00\x00\x14\x80\x01\x00\x15\x02%\x005\x00<\x15"
                b"\x02\x15\xd0\x0f\x15\x90\x03\x15\x00\x00\x1a(\x10INTERFACE_SUBNET\x0eopenr-"
                b'right:lo\x19\x08\x00"(\x05area2\x00'
            ),
            ttl=5793,
            ttlVersion=24,
            hash=-6280819631930425973,
        ),
    },
    expiredKeys=[],
    area="area2",
)

BAD_VALIDATE_TIMESTAMP = 1631215989
# Trailing spaces on the two lines is expected ...
EXPECTED_VALIDATE_OUTPUT_BAD = """\
> Area: area1
> node openr-right's adj db in Decision out of sync with KvStore's 


NEIGHBOR_UPDATE: openr-center via right0
---------  ----------  ---  ----------
timestamp  1631215989  -->  1631215060
---------  ----------  ---  ----------
PASS
adj table for Decision and KvStore match
PASS
prefix table for Decision and KvStore match
> Area: area2
> node openr-right's adj db in Decision out of sync with KvStore's 


NEIGHBOR_UPDATE: openr-center via right0
---------  ----------  ---  ----------
timestamp  1631215989  -->  1631215060
---------  ----------  ---  ----------
PASS
adj table for Decision and KvStore match
PASS
prefix table for Decision and KvStore match
"""

EXPECTED_VALIDATE_OUTPUT_OK = """\
> Area: area1
PASS
adj table for Decision and KvStore match
PASS
prefix table for Decision and KvStore match
> Area: area2
PASS
adj table for Decision and KvStore match
PASS
prefix table for Decision and KvStore match
"""
