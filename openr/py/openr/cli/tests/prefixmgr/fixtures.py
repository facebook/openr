#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-ignore-all-errors
from typing import Dict

from openr.thrift.KvStore import thrift_types as openr_kvstore_types
from openr.thrift.Network.thrift_types import BinaryAddress, IpPrefix, PrefixType
from openr.thrift.OpenrConfig.thrift_types import (
    PrefixForwardingAlgorithm,
    PrefixForwardingType,
)
from openr.thrift.OpenrCtrl.thrift_types import AdvertisedRoute, AdvertisedRouteDetail
from openr.thrift.Types.thrift_types import PrefixEntry, PrefixMetrics

MOCKED_ADVERTISED_ROUTES = [
    AdvertisedRouteDetail(
        prefix=IpPrefix(
            prefixAddress=BinaryAddress(
                addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            ),
            prefixLength=0,
        ),
        bestKey=PrefixType.BGP,
        bestKeys=[PrefixType.BGP],
        routes=[
            AdvertisedRoute(
                key=PrefixType.BGP,
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        ),
                        prefixLength=0,
                    ),
                    type=PrefixType.BGP,
                    forwardingType=PrefixForwardingType.SR_MPLS,
                    forwardingAlgorithm=PrefixForwardingAlgorithm.SP_ECMP,
                    minNexthop=24,
                    metrics=PrefixMetrics(
                        version=1,
                        path_preference=1000,
                        source_preference=100,
                        distance=4,
                    ),
                    tags={"65527:896", "65520:822", "65529:15990", "COMMODITY:EGRESS"},
                    area_stack=["64984", "65333", "64900", "65301"],
                ),
            )
        ],
    ),
    AdvertisedRouteDetail(
        prefix=IpPrefix(
            prefixAddress=BinaryAddress(addr=b"\x00\x00\x00\x00"), prefixLength=0
        ),
        bestKey=PrefixType.BGP,
        bestKeys=[PrefixType.BGP],
        routes=[
            AdvertisedRoute(
                key=PrefixType.BGP,
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(addr=b"\x00\x00\x00\x00"),
                        prefixLength=0,
                    ),
                    type=PrefixType.BGP,
                    forwardingType=PrefixForwardingType.SR_MPLS,
                    forwardingAlgorithm=PrefixForwardingAlgorithm.SP_ECMP,
                    minNexthop=24,
                    metrics=PrefixMetrics(
                        version=1,
                        path_preference=1000,
                        source_preference=100,
                        distance=4,
                    ),
                    tags={"65527:896", "65520:822", "65529:15990", "COMMODITY:EGRESS"},
                    area_stack=["64984", "65333", "64900", "65301"],
                ),
            )
        ],
    ),
]

ADVERTISED_ROUTES_OUTPUT = """\
Markers: * - Best entries (used for forwarding), @ - Entry used to advertise across area
Acronyms: SP - Source Preference, PP - Path Preference, D - Distance
          MN - Min-Nexthops

   Source                               FwdAlgo      FwdType  SP     PP     D      MN

> ::/0, 1/1
*@ BGP                                  SP_ECMP      SR_MPLS  100    1000   4      24

> 0.0.0.0/0, 1/1
*@ BGP                                  SP_ECMP      SR_MPLS  100    1000   4      24

"""

ADVERTISED_ROUTES_OUTPUT_DETAILED = """\
Markers: * - Best entries (used for forwarding), @ - Entry used to advertise across area

> ::/0, 1/1
*@ from BGP
     Forwarding - algorithm: SP_ECMP, type: SR_MPLS
     Metrics - path-preference: 1000, source-preference: 100, distance: 4, drained-path: 0
     Performance - min-nexthops: 24
     Tags - (NA)/65527:896, (NA)/65529:15990, (NA)/COMMODITY:EGRESS, TAG_NAME2/65520:822
     Area Stack - 64984, 65333, 64900, 65301
     IGP Cost - 0

> 0.0.0.0/0, 1/1
*@ from BGP
     Forwarding - algorithm: SP_ECMP, type: SR_MPLS
     Metrics - path-preference: 1000, source-preference: 100, distance: 4, drained-path: 0
     Performance - min-nexthops: 24
     Tags - (NA)/65527:896, (NA)/65529:15990, (NA)/COMMODITY:EGRESS, TAG_NAME2/65520:822
     Area Stack - 64984, 65333, 64900, 65301
     IGP Cost - 0

"""

# Keeping for reference
ADVERTISED_ROUTES_OUTPUT_JSON = """\
[
  {
    "bestKey": 3,
    "bestKeys": [
      3
    ],
    "prefix": "::/0",
    "routes": [
      {
        "hitPolicy": null,
        "igpCost": 0,
        "key": 3,
        "route": {
          "area_stack": [
            "64984",
            "65333",
            "64900",
            "65301"
          ],
          "forwardingAlgorithm": 0,
          "forwardingType": 1,
          "metrics": {
            "distance": 4,
            "drain_metric": 0,
            "path_preference": 1000,
            "source_preference": 100,
            "version": 1
          },
          "minNexthop": 24,
          "prefix": "::/0",
          "tags": [
            "65520:822",
            "65527:896",
            "65529:15990",
            "COMMODITY:EGRESS"
          ],
          "type": 3,
          "weight": null
        }
      }
    ]
  },
  {
    "bestKey": 3,
    "bestKeys": [
      3
    ],
    "prefix": "0.0.0.0/0",
    "routes": [
      {
        "hitPolicy": null,
        "igpCost": 0,
        "key": 3,
        "route": {
          "area_stack": [
            "64984",
            "65333",
            "64900",
            "65301"
          ],
          "forwardingAlgorithm": 0,
          "forwardingType": 1,
          "metrics": {
            "distance": 4,
            "drain_metric": 0,
            "path_preference": 1000,
            "source_preference": 100,
            "version": 1
          },
          "minNexthop": 24,
          "prefix": "0.0.0.0/0",
          "tags": [
            "65520:822",
            "65527:896",
            "65529:15990",
            "COMMODITY:EGRESS"
          ],
          "type": 3,
          "weight": null
        }
      }
    ]
  }
]
"""

MOCKED_INIT_EVENT_GOOD: dict[openr_kvstore_types.InitializationEvent, int] = {
    openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED: 9206,
}

MOCKED_INIT_EVENT_WARNING: dict[openr_kvstore_types.InitializationEvent, int] = {
    openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED: 170000,
}

MOCKED_INIT_EVENT_TIMEOUT: dict[openr_kvstore_types.InitializationEvent, int] = {
    openr_kvstore_types.InitializationEvent.PREFIX_DB_SYNCED: 300000,
}
