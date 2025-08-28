#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import ipaddress
import socket
from typing import List, Optional, Union

import openr.thrift.Network.thrift_types as network_types
import openr.thrift.OpenrConfig.thrift_types as openr_config_types
import openr.thrift.Types.thrift_types as openr_types

# Keep backward compatibility during migration
from openr.Network import ttypes as network_types_deprecated
from openr.thrift.Network.thrift_types import (
    BinaryAddress,
    IpPrefix,
    MplsAction,
    MplsActionCode,
    MplsRoute,
    NextHopThrift,
    UnicastRoute,
)


def sprint_addr(addr: bytes) -> str:
    """binary ip addr -> string"""

    if not len(addr) or not addr:
        return ""

    return str(ipaddress.ip_address(addr))


def sprint_prefix(
    prefix: IpPrefix | network_types.IpPrefix | network_types_deprecated.IpPrefix,
) -> str:
    """
    :param prefix: network_types.IpPrefix representing an CIDR network

    :returns: string representation of prefix (CIDR network)
    :rtype: str or unicode
    """

    return f"{sprint_addr(prefix.prefixAddress.addr)}/{prefix.prefixLength}"


def ip_str_to_addr(addr_str: str, if_index: str | None = None) -> BinaryAddress:
    """
    :param addr_str: ip address in string representation

    :returns: thrift-python struct BinaryAddress
    :rtype: BinaryAddress
    """

    # Try v4
    try:
        addr = socket.inet_pton(socket.AF_INET, addr_str)
        return BinaryAddress(addr=addr, ifName=if_index)
    except OSError:
        pass

    # Try v6
    addr = socket.inet_pton(socket.AF_INET6, addr_str)
    return BinaryAddress(addr=addr, ifName=if_index)


# to be deprecated
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


def ip_str_to_prefix(prefix_str: str) -> IpPrefix:
    """
    :param prefix_str: string representing a prefix (CIDR network)

    :returns: thrift-python struct IpPrefix
    :rtype: IpPrefix
    """

    ip_str, ip_len_str = prefix_str.split("/")
    return IpPrefix(prefixAddress=ip_str_to_addr(ip_str), prefixLength=int(ip_len_str))


# to be deprecated
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


def ip_nexthop_to_nexthop_thrift(
    ip_addr: str, if_index: str, weight: int = 0, metric: int = 0
) -> NextHopThrift:
    """
    :param ip_addr: Next hop IP address
    :param if_index: Next hop interface index
    :param weight: Next hop weigth
    :param metric: Cost associated with next hop

    :rtype: NextHopThrift (thrift-python)
    """

    binary_address = ip_str_to_addr(ip_addr, if_index)
    return NextHopThrift(address=binary_address, weight=weight, metric=metric)


def ip_to_unicast_route(ip_prefix: str, nexthops: list[NextHopThrift]) -> UnicastRoute:
    """
    :param ip_prefix: IP prefix
    :param nexthops: List of next hops

    :rtype: UnicastRoute (thrift-python)
    """

    return UnicastRoute(dest=ip_str_to_prefix(ip_prefix), nextHops=nexthops)


def mpls_to_mpls_route(label: int, nexthops: list[NextHopThrift]) -> MplsRoute:
    """
    :param label: MPLS label
    :param nexthops: List of nexthops

    :rtype: MplsRoute (thrift-python)
    """
    return MplsRoute(topLabel=label, nextHops=nexthops)


def mpls_nexthop_to_nexthop_thrift(
    ip_addr: str,
    if_index: str,
    weight: int = 0,
    metric: int = 0,
    label: list[int] | None = None,
    action: MplsActionCode = MplsActionCode.PHP,
) -> NextHopThrift:
    """
    :param label: label(s) for PUSH, SWAP action
    :param action: label action PUSH, POP, SWAP
    :param ip_addr: Next hop IP address
    :param if_index: Next hop interface index
    :param weight: Next hop weigth
    :param metric: Cost associated with next hop

    :rtype: NextHopThrift (thrift-python)
    """

    binary_address = ip_str_to_addr(ip_addr, if_index)
    mpls_action = MplsAction(action=action)
    if action == MplsActionCode.SWAP:
        # pyre-fixme[16]: `Optional` has no attribute `__getitem__`.
        mpls_action = mpls_action(swapLabel=label[0])
    elif action == MplsActionCode.PUSH:
        mpls_action = mpls_action(pushLabels=label[:])

    return NextHopThrift(
        address=binary_address, weight=weight, metric=metric, mplsAction=mpls_action
    )


def routes_to_route_db(
    node: str,
    unicast_routes: list[network_types.UnicastRoute] | None = None,
    mpls_routes: list[network_types.MplsRoute] | None = None,
) -> openr_types.RouteDatabase:
    """
    :param node: node name
    :param unicast_routes: list of unicast IP routes
    :param mpls_routes: list of MPLS routes
    """
    unicast_routes = [] if unicast_routes is None else unicast_routes
    mpls_routes = [] if mpls_routes is None else mpls_routes

    return openr_types.RouteDatabase(
        thisNodeName=node, unicastRoutes=unicast_routes, mplsRoutes=mpls_routes
    )


def sprint_prefix_type(prefix_type):
    """
    :param prefix: network_types.PrefixType
    """
    if isinstance(prefix_type, network_types.PrefixType):
        return prefix_type.name
    try:
        return network_types.PrefixType(prefix_type).name
    except ValueError:
        return None


def sprint_prefix_forwarding_type(forwarding_type):
    """
    :param forwarding_type: openr_config_types.PrefixForwardingType
    """
    if isinstance(forwarding_type, openr_config_types.PrefixForwardingType):
        return forwarding_type.name
    try:
        return openr_config_types.PrefixForwardingType(forwarding_type).name
    except ValueError:
        return None


def sprint_prefix_forwarding_algorithm(
    forwarding_algo: openr_config_types.PrefixForwardingAlgorithm,
) -> str | None:
    """
    :param forwarding_algorithm: openr_config_types.PrefixForwardingAlgorithm
    """
    if isinstance(forwarding_algo, openr_config_types.PrefixForwardingAlgorithm):
        return forwarding_algo.name
    try:
        return openr_config_types.PrefixForwardingAlgorithm(forwarding_algo).name
    except ValueError:
        return None


def ip_version(addr: object):
    """return ip addr version"""

    # pyre-fixme[6]: For 1st param expected `Union[bytes, int, IPv4Address,
    #  IPv6Address, str]` but got `object`.
    return ipaddress.ip_address(addr).version


def is_same_subnet(addr1, addr2, subnet) -> bool:
    """
    Check whether two given addresses belong to the same subnet
    """

    if ipaddress.ip_network((addr1, subnet), strict=False) == ipaddress.ip_network(
        (addr2, subnet),
        strict=False,
    ):
        return True

    return False


def is_link_local(addr: object):
    """
    Check whether given addr is link local or not
    """
    # pyre-fixme[6]: For 1st param expected `Union[bytes, int, IPv4Address,
    #  IPv4Interface, IPv4Network, IPv6Address, IPv6Interface, IPv6Network, str]` but
    #  got `object`.
    return ipaddress.ip_network(addr).is_link_local


def is_subnet_of(a, b) -> bool:
    """
    Check if network-b is subnet of network-a
    """

    if a.network_address != b.network_address:
        return False

    return a.prefixlen >= b.prefixlen


def contain_any_prefix(prefix, ip_networks) -> bool:
    """
    Utility function to check if prefix contain any of the prefixes/ips

    :returns: True if prefix contains any of the ip_networks else False
    """

    if ip_networks is None:
        return True
    prefix = ipaddress.ip_network(prefix)
    return any(is_subnet_of(prefix, net) for net in ip_networks)


def is_ip_addr(addr: str, strict: bool = True) -> bool:
    """
    Checks if a string is a valid IPv4 or IPv6 address,
    If Strict is set to True, will return False if there's
    mask/host bits
    """

    try:
        ipaddress.ip_network(addr, strict=strict)
        return True
    except ValueError:
        return False
