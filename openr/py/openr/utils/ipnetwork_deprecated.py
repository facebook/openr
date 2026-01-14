#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import socket

from openr.Network import ttypes as network_types
from openr.OpenrConfig import ttypes as openr_config_types
from openr.py.openr.utils.ipnetwork import sprint_addr
from openr.thrift.Network import types as network_types_py3
from openr.thrift.Network.thrift_types import IpPrefix
from openr.Types import ttypes as openr_types


def sprint_prefix(
    prefix: IpPrefix | network_types_py3.IpPrefix | network_types.IpPrefix,
) -> str:
    """
    :param prefix: network_types.IpPrefix representing an CIDR network

    :returns: string representation of prefix (CIDR network)
    :rtype: str or unicode
    """

    return f"{sprint_addr(prefix.prefixAddress.addr)}/{prefix.prefixLength}"


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


def sprint_prefix_type(prefix_type):
    """
    :param prefix: network_types.PrefixType
    """

    return network_types.PrefixType._VALUES_TO_NAMES.get(prefix_type, None)


def sprint_prefix_forwarding_type(forwarding_type):
    """
    :param forwarding_type: openr_config_types.PrefixForwardingType
    """

    return openr_config_types.PrefixForwardingType._VALUES_TO_NAMES.get(forwarding_type)


def sprint_prefix_forwarding_algorithm(
    forwarding_algo: openr_config_types.PrefixForwardingAlgorithm,
) -> str | None:
    """
    :param forwarding_algorithm: openr_config_types.PrefixForwardingAlgorithm
    """
    return openr_config_types.PrefixForwardingAlgorithm._VALUES_TO_NAMES.get(
        forwarding_algo
    )
