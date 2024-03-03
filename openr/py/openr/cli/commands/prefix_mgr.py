#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from typing import Dict, List, Optional, Sequence, Tuple

from openr.cli.utils import utils

from openr.cli.utils.commands import OpenrCtrlCmd
from openr.cli.utils.utils import (
    get_tag_to_name_map,
    print_advertised_routes,
    print_route_details,
    PrintAdvertisedTypes,
)
from openr.thrift.KvStore.thrift_types import InitializationEvent
from openr.thrift.Network.thrift_types import PrefixType
from openr.thrift.OpenrConfig.thrift_types import PrefixForwardingType
from openr.thrift.OpenrCtrl import thrift_types as ctrl_types
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Types.thrift_types import OriginatedPrefixEntry, PrefixEntry
from openr.utils import ipnetwork, serializer


def prefix_type_key_fn(key: PrintAdvertisedTypes) -> Tuple[str]:
    try:
        return (key.name if key in PrefixType else "N/A - Not in PrefixType enum",)
    except ValueError:
        pass  # Here we should have debug logging ...
    return ("N/A - ERROR",)


def get_advertised_route_filter(
    prefixes: List[str], prefix_type: Optional[str]
) -> ctrl_types.AdvertisedRouteFilter:
    return ctrl_types.AdvertisedRouteFilter(
        prefixes=(
            [ipnetwork.ip_str_to_prefix(p) for p in prefixes] if prefixes else None
        ),
        prefixType=to_thrift_prefix_type(prefix_type) if prefix_type else None,
    )


def to_thrift_prefix_type(prefix_type: str) -> PrefixType:
    PREFIX_TYPE_TO_VALUES = {e.name: e for e in PrefixType}
    if prefix_type.upper() not in PREFIX_TYPE_TO_VALUES:
        raise Exception(
            "Unknown type {}. Use any of {}".format(
                prefix_type, ", ".join(PREFIX_TYPE_TO_VALUES.keys())
            )
        )

    return PREFIX_TYPE_TO_VALUES[prefix_type.upper()]


class PrefixMgrCmd(OpenrCtrlCmd):
    pass


class AdvertisedRoutesCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        routes = await self.fetch(client, prefixes, prefix_type)

        # Print json if
        if json:
            print(serializer.serialize_json(routes))
        else:
            await self.render(routes, detailed)

    async def fetch(
        self,
        client: OpenrCtrlCppClient.Async,
        prefixes: List[str],
        prefix_type: Optional[str],
    ) -> Sequence[ctrl_types.AdvertisedRouteDetail]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = get_advertised_route_filter(prefixes, prefix_type)

        # Get routes
        return await client.getAdvertisedRoutesFiltered(route_filter)

    async def render(
        self, routes: Sequence[ctrl_types.AdvertisedRouteDetail], detailed: bool
    ) -> None:
        """
        Render advertised routes
        """
        tag_to_name = None
        if self.cli_opts["advertised_routes_options"]["tag2name"]:
            tag_to_name = get_tag_to_name_map(await self._get_config())
        print_route_details(routes, prefix_type_key_fn, detailed, tag_to_name)


class OriginatedRoutesCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        detailed: bool,
        tag2name: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        await self._render(await client.getOriginatedPrefixes(), detailed, tag2name)

    async def _render(
        self,
        originated_prefixes: Sequence[OriginatedPrefixEntry],
        detailed: bool,
        tag2name: bool,
    ) -> None:
        """
        Render advertised routes
        """
        rows = [""]

        tag_to_name = (
            get_tag_to_name_map(await self._get_config()) if tag2name else None
        )
        if detailed:
            self._print_orig_routes_detailed(rows, originated_prefixes, tag_to_name)
        else:
            self._print_orig_routes(rows, originated_prefixes, tag_to_name)

        print("\n".join(rows))

    def _print_orig_routes(
        self,
        rows: List[str],
        originated_prefixes: Sequence[OriginatedPrefixEntry],
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
        originated_prefixes: Sequence[OriginatedPrefixEntry],
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
            fwd_algo = prefix_entry.prefix.forwardingAlgorithm.name
            fwd_type = prefix_entry.prefix.forwardingType.name
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
                    f"     Tags - {', '.join([utils.format_openr_tag(t,tag_to_name) for t in prefix_tags])}"
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
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
        *args,
        **kwargs,
    ) -> None:

        # Get data
        routes = await self.fetch(client, route_filter_type, prefixes, prefix_type)

        # Print json if
        if json:
            print(serializer.serialize_json(routes))
        else:
            await self.render(routes, detailed)

    async def fetch(
        self,
        client: OpenrCtrlCppClient.Async,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
    ) -> Sequence[ctrl_types.AdvertisedRoute]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = get_advertised_route_filter(prefixes, prefix_type)

        # Get routes
        return await client.getAdvertisedRoutesWithOriginationPolicy(
            route_filter_type, route_filter
        )

    async def render(
        self, routes: Sequence[ctrl_types.AdvertisedRoute], detailed: bool
    ) -> None:
        """
        Render advertised routes
        """
        tag_to_name = None
        if self.cli_opts["advertised_routes_options"]["tag2name"]:
            tag_to_name = get_tag_to_name_map(await self._get_config())
        print_advertised_routes(routes, prefix_type_key_fn, detailed, tag_to_name)


class AreaAdvertisedRoutesCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        area: str,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
        *args,
        **kwargs,
    ) -> None:

        # Deduce / Validate area
        area = await utils.deduce_area(client, area)

        # Get data
        routes = await self.fetch(
            client, area, route_filter_type, prefixes, prefix_type
        )

        # Print json if
        if json:
            print(serializer.serialize_json(routes))
        else:
            await self.render(routes, detailed)

    async def fetch(
        self,
        client: OpenrCtrlCppClient.Async,
        area: str,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
    ) -> Sequence[ctrl_types.AdvertisedRoute]:
        """
        Fetch the requested data
        """

        # Create filter
        route_filter = get_advertised_route_filter(prefixes, prefix_type)

        # Get routes
        return await client.getAreaAdvertisedRoutesFiltered(
            area, route_filter_type, route_filter
        )

    async def render(
        self, routes: Sequence[ctrl_types.AdvertisedRoute], detailed: bool
    ) -> None:
        """
        Render advertised routes
        """
        tag_to_name = None
        if self.cli_opts["advertised_routes_options"]["tag2name"]:
            tag_to_name = get_tag_to_name_map(await self._get_config())
        print_advertised_routes(routes, prefix_type_key_fn, detailed, tag_to_name)


class ValidateCmd(PrefixMgrCmd):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        *args,
        **kwargs,
    ) -> bool:

        is_pass = True

        # Get Data
        initialization_events = await client.getInitializationEvents()

        # Run Validation Checks
        init_is_pass, init_err_msg_str = self.validate_init_event(
            initialization_events,
            InitializationEvent.PREFIX_DB_SYNCED,
        )

        is_pass = is_pass and init_is_pass

        # Print Validation Check Results
        self.print_initialization_event_check(
            init_is_pass,
            init_err_msg_str,
            InitializationEvent.PREFIX_DB_SYNCED,
            "PrefixManager",
        )

        return is_pass
