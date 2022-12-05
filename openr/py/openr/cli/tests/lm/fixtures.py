#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict


from openr.thrift.Network.thrift_types import BinaryAddress, IpPrefix
from openr.thrift.Types.thrift_types import (
    DumpLinksReply,
    InterfaceDetails,
    InterfaceInfo,
)

LM_LINKS_OPENR_RIGHT_OK = DumpLinksReply(
    thisNodeName="openr-right",
    nodeMetricIncrementVal=0,
    isOverloaded=False,
    interfaceDetails={
        "lo": InterfaceDetails(
            info=InterfaceInfo(
                isUp=True,
                ifIndex=1,
                networks=[
                    IpPrefix(
                        prefixAddress=BinaryAddress(addr=b"\x7f\x00\x00\x01"),
                        prefixLength=8,
                    ),
                    IpPrefix(
                        prefixAddress=BinaryAddress(addr=b"\n\x06\t\x02"),
                        prefixLength=32,
                    ),
                    IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\xfd\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        ),
                        prefixLength=64,
                    ),
                    IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01"
                        ),
                        prefixLength=128,
                    ),
                ],
            ),
            isOverloaded=False,
            linkMetricIncrementVal=0,
        ),
        "right0": InterfaceDetails(
            info=InterfaceInfo(
                isUp=True,
                ifIndex=63,
                networks=[
                    IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\xfd\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02"
                        ),
                        prefixLength=64,
                    ),
                    IpPrefix(
                        prefixAddress=BinaryAddress(
                            addr=b"\xfe\x80\x00\x00\x00\x00\x00\x00P\xef\xde\xff\xfeL\xd4\xf6"
                        ),
                        prefixLength=64,
                    ),
                ],
            ),
            isOverloaded=False,
            linkMetricIncrementVal=0,
        ),
    },
)

LM_LINKS_EXPECTED_OPENR_RIGHT_STDOUT = """
== Node Overload: NO, Node Metric Increment: 0  ==

Interface    Status    Metric Override    Addresses
-----------  --------  -----------------  -------------------------
lo           Up
                                          127.0.0.1
                                          10.6.9.2
                                          fd00:2::
                                          ::1
right0       Up
                                          fd00::2
                                          fe80::50ef:deff:fe4c:d4f6
"""
