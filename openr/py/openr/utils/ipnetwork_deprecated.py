#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import socket

from openr.Network import ttypes as network_types


def ip_str_to_addr_py(
    addr_str: str, if_index: str | None = None
) -> network_types.BinaryAddress:
    """
    :param addr_str: ip address in string representation

    :returns: thrift-py struct BinaryAddress
    :rtype: network_types.BinaryAddress
    """

    # Try v4
    try:
        addr = socket.inet_pton(socket.AF_INET, addr_str)
        binary_address = network_types.BinaryAddress(addr=addr, ifName=if_index or None)
        return binary_address
    except OSError:
        pass

    # Try v6
    addr = socket.inet_pton(socket.AF_INET6, addr_str)
    binary_address = network_types.BinaryAddress(addr=addr, ifName=if_index or None)
    return binary_address


def ip_str_to_prefix_py(prefix_str: str) -> network_types.IpPrefix:
    """
    :param prefix_str: string representing a prefix (CIDR network)

    :returns: thrift-py struct IpPrefix
    :rtype: network_types.IpPrefix
    """

    ip_str, ip_len_str = prefix_str.split("/")
    return network_types.IpPrefix(
        prefixAddress=ip_str_to_addr_py(ip_str), prefixLength=int(ip_len_str)
    )
