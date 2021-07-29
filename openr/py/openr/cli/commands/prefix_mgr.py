#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from typing import Dict, List, Optional, Tuple

from openr.cli.utils.commands import OpenrCtrlCmd
from openr.cli.utils.utils import (
    get_tag_to_name_map,
    PrintAdvertisedTypes,
    print_route_details,
    print_advertised_routes,
)
from openr.Network import ttypes as network_types
from openr.OpenrConfig.ttypes import PrefixForwardingType, PrefixForwardingAlgorithm
from openr.OpenrCtrl import OpenrCtrl, ttypes as ctrl_types
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing


def prefix_type_key_fn(key: PrintAdvertisedTypes) -> Tuple[str]:
    if not isinstance(key, network_types.PrefixType):
        return ("N/A",)
    return (network_types.PrefixType._VALUES_TO_NAMES.get(key, "N/A"),)


def get_advertised_route_filter(
    prefixes: List[str], prefix_type: Optional[str]
) -> ctrl_types.AdvertisedRouteFilter:
    route_filter = ctrl_types.AdvertisedRouteFilter()
    if prefixes:
        route_filter.prefixes = [ipnetwork.ip_str_to_prefix(p) for p in prefixes]
    if prefix_type:
        route_filter.prefixType = to_thrift_prefix_type(prefix_type)
    return route_filter


def to_thrift_prefixes(
    prefixes: List[str],
    prefix_type: network_types.PrefixType,
    forwarding_type: PrefixForwardingType = PrefixForwardingType.IP,
) -> List[openr_types.PrefixEntry]:
    return [
        openr_types.PrefixEntry(
            prefix=ipnetwork.ip_str_to_prefix(prefix),
            type=prefix_type,
            forwardingType=forwarding_type,
        )
        for prefix in prefixes
    ]


def to_thrift_prefix_type(prefix_type: str) -> network_types.PrefixType:
    PREFIX_TYPE_TO_VALUES = network_types.PrefixType._NAMES_TO_VALUES
    if prefix_type.upper() not in PREFIX_TYPE_TO_VALUES:
        raise Exception(
            "Unknown type {}. Use any of {}".format(
                prefix_type, ", ".join(PREFIX_TYPE_TO_VALUES.keys())
            )
        )

    return PREFIX_TYPE_TO_VALUES[prefix_type.upper()]


def to_thrift_forwarding_type(forwarding_type: str) -> PrefixForwardingType:
    FORWARDING_TYPE_TO_VALUES = PrefixForwardingType._NAMES_TO_VALUES
    if forwarding_type not in FORWARDING_TYPE_TO_VALUES:
        raise Exception(
            "Unknown forwarding type {}. Use any of {}".format(
                forwarding_type, ", ".join(FORWARDING_TYPE_TO_VALUES.keys())
            )
        )
    return FORWARDING_TYPE_TO_VALUES[forwarding_type]


class PrefixMgrCmd(OpenrCtrlCmd):
    pass


class WithdrawCmd(PrefixMgrCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        prefix_type: str,
        *args,
        **kwargs,
    ) -> None:
        tprefixes = to_thrift_prefixes(prefixes, to_thrift_prefix_type(prefix_type))
        client.withdrawPrefixes(tprefixes)
        print(f"Withdrew {len(prefixes)} prefixes")


class AdvertiseCmd(PrefixMgrCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        prefix_type: str,
        forwarding_type: str,
        *args,
        **kwargs,
    ) -> None:
        tprefixes = to_thrift_prefixes(
            prefixes,
            to_thrift_prefix_type(prefix_type),
            to_thrift_forwarding_type(forwarding_type),
        )
        client.advertisePrefixes(tprefixes)
        print(f"Advertised {len(prefixes)} prefixes with type {prefix_type}")


class SyncCmd(PrefixMgrCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        prefix_type: str,
        forwarding_type: str,
        *args,
        **kwargs,
    ) -> None:
        tprefix_type = to_thrift_prefix_type(prefix_type)
        tprefixes = to_thrift_prefixes(
            prefixes, tprefix_type, to_thrift_forwarding_type(forwarding_type)
        )
        client.syncPrefixesByType(tprefix_type, tprefixes)
        print(f"Synced {len(prefixes)} prefixes with type {prefix_type}")


class ViewCmd(PrefixMgrCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        *args,
        **kwargs,
    ) -> None:
        prefixes = client.getPrefixes()
        rows = []
        for prefix_entry in prefixes:
            prefix_str = ipnetwork.sprint_prefix(prefix_entry.prefix)
            prefix_type = ipnetwork.sprint_prefix_type(prefix_entry.type)
            forwarding_type = ipnetwork.sprint_prefix_forwarding_type(
                prefix_entry.forwardingType
            )
            rows.append((prefix_type, prefix_str, forwarding_type))
        print(
            "\n",
            printing.render_horizontal_table(
                rows, ["Type", "Prefix", "Forwarding Type"]
            ),
        )
        print()


class AdvertisedRoutesCmd(PrefixMgrCmd):

    # @override
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        routes = self.fetch(client, prefixes, prefix_type)

        # Print json if
        if json:
            # TODO: Print routes in json
            raise NotImplementedError()
        else:
            self.render(routes, detailed)

    def fetch(
        self, client: OpenrCtrl.Client, prefixes: List[str], prefix_type: Optional[str]
    ) -> List[ctrl_types.AdvertisedRouteDetail]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = get_advertised_route_filter(prefixes, prefix_type)

        # Get routes
        return client.getAdvertisedRoutesFiltered(route_filter)

    def render(
        self, routes: List[ctrl_types.AdvertisedRouteDetail], detailed: bool
    ) -> None:
        """
        Render advertised routes
        """
        tag_to_name = None
        if self.cli_opts["advertised_routes_options"]["tag2name"]:
            tag_to_name = get_tag_to_name_map(self._get_config())
        print_route_details(routes, prefix_type_key_fn, detailed, tag_to_name)


class OriginatedRoutesCmd(PrefixMgrCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        detailed: bool,
        tag2name: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        self._render(client.getOriginatedPrefixes(), detailed, tag2name)

    def _render(
        self,
        originated_prefixes: List[openr_types.OriginatedPrefixEntry],
        detailed: bool,
        tag2name: bool,
    ) -> None:
        """
        Render advertised routes
        """
        rows = [""]

        tag_to_name = get_tag_to_name_map(self._get_config()) if tag2name else None
        if detailed:
            self._print_orig_routes_detailed(rows, originated_prefixes, tag_to_name)
        else:
            self._print_orig_routes(rows, originated_prefixes, tag_to_name)

        print("\n".join(rows))

    def _print_orig_routes(
        self,
        rows: List[str],
        originated_prefixes: List[openr_types.OriginatedPrefixEntry],
        tag_to_name: Optional[Dict[str, str]] = None,
    ):
        """
        Construct print lines of originated route in tabular fashion
        if tag_to_name is provided, translate tag/comm value to its name

        Output format example with table heading:
        Prefix                                     Community     SR    MSR      I
        2401:db00:30:40::/59                     65529:26730      2      1    1/1
                                        FABRIC_POD_CLUSTER_PRIVATE_SUBAGG (if tag_to_name is providied)

        """
        rows.append(
            "Acronyms: SR - Supporting Routes Count | MSR - Min Supporting Routes\n"
            "          I  - Install_to_fib/Installed"
        )
        rows.append("")

        rows.append(
            f"{'Prefix':<36} "
            f"{'Community':>15} "
            f"{'SR':>6} "
            f"{'MSR':>6} "
            f"{'I':>6} "
        )
        rows.append("")
        tag_to_name = tag_to_name if tag_to_name is not None else {}
        for prefix_entry in originated_prefixes:
            tag: List[str] = [""]
            prefix_tags = prefix_entry.prefix.tags
            if prefix_tags:
                tag = [tag_to_name.get(t, t) for t in prefix_tags]

            installed: bool = False
            if prefix_entry.prefix.install_to_fib:
                installed = prefix_entry.installed

            rows.append(
                f"{prefix_entry.prefix.prefix:<36} "
                f"{str(tag[0]):>15} "
                f"{len(prefix_entry.supporting_prefixes):>6} "
                f"{prefix_entry.prefix.minimum_supporting_routes:>6} {'':3}"
                f"{prefix_entry.prefix.install_to_fib:>1}/{installed:<1}"
            )
            for index in range(1, len(tag)):
                rows.append(f"{'':37}{str(tag[index]):>15} ")

    def _print_orig_routes_detailed(
        self,
        rows: List[str],
        originated_prefixes: List[openr_types.OriginatedPrefixEntry],
        tag_to_name: Optional[Dict[str, str]] = None,
    ) -> None:
        """
        Construct print lines of originated route, append to rows

        Output format
                Prefix <key>
                Forwarding algo=<fwd-algo> type=<fwd-type>
                Metrics - path pref<path-pref> source pref <source-pref>
                Tags <tags?>
                Area Stack <area-stack?>
                Min Next Hops <min-nexthops?>
                Min Supporting Routes <min-supporting-routes>
                Install to FIB <install-to-fib?>
        """
        tag_to_name = tag_to_name if tag_to_name is not None else {}

        for prefix_entry in originated_prefixes:
            rows.append(f"> {prefix_entry.prefix.prefix}")
            fwd_algo = PrefixForwardingAlgorithm._VALUES_TO_NAMES.get(
                prefix_entry.prefix.forwardingAlgorithm
            )
            fwd_type = PrefixForwardingType._VALUES_TO_NAMES.get(
                prefix_entry.prefix.forwardingType
            )
            rows.append(
                f"     Forwarding - algorithm: {fwd_algo:>7} {'Type: ' + fwd_type:>23}"
            )
            rows.append(
                f"     Metrics - path-preference: {prefix_entry.prefix.path_preference}"
                f", source-preference: {prefix_entry.prefix.source_preference}"
            )
            prefix_tags = prefix_entry.prefix.tags
            if prefix_tags:
                rows.append(
                    f"     Tags - {', '.join([tag_to_name.get(t,t) for t in prefix_tags])}"
                )
            area_stack = prefix_entry.prefix.area_stack
            if area_stack:
                rows.append(f"     Area Stack - {', '.join(area_stack)}")
            if prefix_entry.prefix.minNexthop:
                rows.append(f"     Min-nexthops: {prefix_entry.prefix.minNexthop}")
            rows.append(
                f"     Min Supporting Routes - {(prefix_entry.prefix.minimum_supporting_routes)}"
                f"     Supporting Routes Cnt - {len(prefix_entry.supporting_prefixes)}"
            )
            if prefix_entry.prefix.install_to_fib:
                rows.append(
                    f"     install_to_fib: {prefix_entry.prefix.install_to_fib}"
                    f" {'Installed: ' + str(prefix_entry.installed):>34}"
                )

            rows.append("")


class AdvertisedRoutesWithOriginationPolicyCmd(PrefixMgrCmd):

    # @override
    def _run(
        self,
        client: OpenrCtrl.Client,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        routes = self.fetch(client, route_filter_type, prefixes, prefix_type)

        # Print json if
        if json:
            # TODO: Print routes in json
            raise NotImplementedError()
        else:
            self.render(routes, detailed)

    def fetch(
        self,
        client: OpenrCtrl.Client,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
    ) -> List[ctrl_types.AdvertisedRoute]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = get_advertised_route_filter(prefixes, prefix_type)

        # Get routes
        return client.getAdvertisedRoutesWithOriginationPolicy(
            route_filter_type, route_filter
        )

    def render(self, routes: List[ctrl_types.AdvertisedRoute], detailed: bool) -> None:
        """
        Render advertised routes
        """
        tag_to_name = None
        if self.cli_opts["advertised_routes_options"]["tag2name"]:
            tag_to_name = get_tag_to_name_map(self._get_config())
        print_advertised_routes(routes, prefix_type_key_fn, detailed, tag_to_name)


class AreaAdvertisedRoutesCmd(PrefixMgrCmd):

    # @override
    def _run(
        self,
        client: OpenrCtrl.Client,
        area: str,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        routes = self.fetch(client, area, route_filter_type, prefixes, prefix_type)

        # Print json if
        if json:
            # TODO: Print routes in json
            raise NotImplementedError()
        else:
            self.render(routes, detailed)

    def fetch(
        self,
        client: OpenrCtrl.Client,
        area: str,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
    ) -> List[ctrl_types.AdvertisedRoute]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = get_advertised_route_filter(prefixes, prefix_type)

        # Get routes
        return client.getAreaAdvertisedRoutesFiltered(
            area, route_filter_type, route_filter
        )

    def render(self, routes: List[ctrl_types.AdvertisedRoute], detailed: bool) -> None:
        """
        Render advertised routes
        """
        tag_to_name = None
        if self.cli_opts["advertised_routes_options"]["tag2name"]:
            tag_to_name = get_tag_to_name_map(self._get_config())
        print_advertised_routes(routes, prefix_type_key_fn, detailed, tag_to_name)
