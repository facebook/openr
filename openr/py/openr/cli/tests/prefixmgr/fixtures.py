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
    AdvertisedRoute,
    AdvertisedRouteDetail,
)
from openr.Types.ttypes import (
    PrefixEntry,
    PrefixMetrics,
)


MOCKED_ADVERTISED_ROUTES = [
    AdvertisedRouteDetail(
        prefix=IpPrefix(
            prefixAddress=BinaryAddress(
                addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            ),
            prefixLength=0,
        ),
        bestKey=3,
        bestKeys=[3],
        routes=[
            AdvertisedRoute(
                key=3,
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        ),
                        prefixLength=0,
                    ),
                    type=3,
                    data=b"b''",
                    forwardingType=1,
                    forwardingAlgorithm=0,
                    minNexthop=24,
                    prependLabel=65001,
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
        bestKey=3,
        bestKeys=[3],
        routes=[
            AdvertisedRoute(
                key=3,
                route=PrefixEntry(
                    prefix=IpPrefix(
                        prefixAddress=BinaryAddress(addr=b"\x00\x00\x00\x00"),
                        prefixLength=0,
                    ),
                    type=3,
                    data=b"b''",
                    forwardingType=1,
                    forwardingAlgorithm=0,
                    minNexthop=24,
                    prependLabel=60000,
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
Markers: * - One of the best entries, @ - The best entry
Acronyms: SP - Source Preference, PP - Path Preference, D - Distance
          MN - Min-Nexthops, PL - Prepend Label

   Source                               FwdAlgo      FwdType  SP     PP     D      MN    PL    

> ::/0, 1/1
*@ BGP                                  SP_ECMP      SR_MPLS  100    1000   4      24    65001 

> 0.0.0.0/0, 1/1
*@ BGP                                  SP_ECMP      SR_MPLS  100    1000   4      24    60000 

"""

ADVERTISED_ROUTES_OUTPUT_DETAILED = """\
Markers: * - One of the best entries, @ - The best entry

> ::/0, 1/1
*@ from BGP
     Forwarding - algorithm: SP_ECMP, type: SR_MPLS
     Metrics - path-preference: 1000, source-preference: 100, distance: 4
     Performance - min-nexthops: 24
     Misc - prepend-label: 65001
     Tags - 65520:822, 65527:896, 65529:15990, COMMODITY:EGRESS
     Area Stack - 64984, 65333, 64900, 65301

> 0.0.0.0/0, 1/1
*@ from BGP
     Forwarding - algorithm: SP_ECMP, type: SR_MPLS
     Metrics - path-preference: 1000, source-preference: 100, distance: 4
     Performance - min-nexthops: 24
     Misc - prepend-label: 60000
     Tags - 65520:822, 65527:896, 65529:15990, COMMODITY:EGRESS
     Area Stack - 64984, 65333, 64900, 65301

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
        "igpCost": null,
        "key": 3,
        "route": {
          "area_stack": [
            "64984",
            "65333",
            "64900",
            "65301"
          ],
          "data": "b''",
          "forwardingAlgorithm": 0,
          "forwardingType": 1,
          "metrics": {
            "distance": 4,
            "path_preference": 1000,
            "source_preference": 100,
            "version": 1
          },
          "minNexthop": 24,
          "mv": null,
          "prefix": "::/0",
          "prependLabel": 65001,
          "tags": [
            "65520:822",
            "65527:896",
            "65529:15990",
            "COMMODITY:EGRESS"
          ],
          "type": 3
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
        "igpCost": null,
        "key": 3,
        "route": {
          "area_stack": [
            "64984",
            "65333",
            "64900",
            "65301"
          ],
          "data": "b''",
          "forwardingAlgorithm": 0,
          "forwardingType": 1,
          "metrics": {
            "distance": 4,
            "path_preference": 1000,
            "source_preference": 100,
            "version": 1
          },
          "minNexthop": 24,
          "mv": null,
          "prefix": "0.0.0.0/0",
          "prependLabel": 60000,
          "tags": [
            "65520:822",
            "65527:896",
            "65529:15990",
            "COMMODITY:EGRESS"
          ],
          "type": 3
        }
      }
    ]
  }
]
"""
