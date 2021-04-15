#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from typing import List, Optional, Tuple

from openr.cli.utils.commands import OpenrCtrlCmd
from openr.cli.utils.utils import print_route_details, print_advertised_routes
from openr.Network import ttypes as network_types
from openr.OpenrConfig.ttypes import PrefixForwardingType, PrefixForwardingAlgorithm
from openr.OpenrCtrl import OpenrCtrl, ttypes as ctrl_types
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing


class PrefixMgrCmd(OpenrCtrlCmd):
    def to_thrift_prefixes(
        self,
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

    def to_thrift_prefix_type(self, prefix_type: str) -> network_types.PrefixType:
        PREFIX_TYPE_TO_VALUES = network_types.PrefixType._NAMES_TO_VALUES
        if prefix_type.upper() not in PREFIX_TYPE_TO_VALUES:
            raise Exception(
                "Unknown type {}. Use any of {}".format(
                    prefix_type, ", ".join(PREFIX_TYPE_TO_VALUES.keys())
                )
            )

        return PREFIX_TYPE_TO_VALUES[prefix_type.upper()]

    def to_thrift_forwarding_type(self, forwarding_type: str) -> PrefixForwardingType:
        FORWARDING_TYPE_TO_VALUES = PrefixForwardingType._NAMES_TO_VALUES
        if forwarding_type not in FORWARDING_TYPE_TO_VALUES:
            raise Exception(
                "Unknown forwarding type {}. Use any of {}".format(
                    forwarding_type, ", ".join(FORWARDING_TYPE_TO_VALUES.keys())
                )
            )
        return FORWARDING_TYPE_TO_VALUES[forwarding_type]


class WithdrawCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self, client: OpenrCtrl.Client, prefixes: List[str], prefix_type: str
    ) -> None:
        tprefixes = self.to_thrift_prefixes(
            prefixes, self.to_thrift_prefix_type(prefix_type)
        )
        client.withdrawPrefixes(tprefixes)
        print(f"Withdrew {len(prefixes)} prefixes")


class AdvertiseCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        prefix_type: str,
        forwarding_type: str,
    ) -> None:
        tprefixes = self.to_thrift_prefixes(
            prefixes,
            self.to_thrift_prefix_type(prefix_type),
            self.to_thrift_forwarding_type(forwarding_type),
        )
        client.advertisePrefixes(tprefixes)
        print(f"Advertised {len(prefixes)} prefixes with type {prefix_type}")


class SyncCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        prefix_type: str,
        forwarding_type: str,
    ) -> None:
        tprefix_type = self.to_thrift_prefix_type(prefix_type)
        tprefixes = self.to_thrift_prefixes(
            prefixes, tprefix_type, self.to_thrift_forwarding_type(forwarding_type)
        )
        client.syncPrefixesByType(tprefix_type, tprefixes)
        print(f"Synced {len(prefixes)} prefixes with type {prefix_type}")


class ViewCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client) -> None:
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
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self,
        client: OpenrCtrl.Client,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
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
        route_filter = ctrl_types.AdvertisedRouteFilter()
        if prefixes:
            route_filter.prefixes = [ipnetwork.ip_str_to_prefix(p) for p in prefixes]
        if prefix_type:
            route_filter.prefixType = self.to_thrift_prefix_type(prefix_type)

        # Get routes
        return client.getAdvertisedRoutesFiltered(route_filter)

    def render(
        self, routes: List[ctrl_types.AdvertisedRouteDetail], detailed: bool
    ) -> None:
        """
        Render advertised routes
        """

        def key_fn(key: network_types.PrefixType) -> Tuple[str]:
            return (network_types.PrefixType._VALUES_TO_NAMES.get(key, "N/A"),)

        # pyre-fixme[6]: Expected
        #  `List[typing.Union[ctrl_types.AdvertisedRouteDetail,
        #  ctrl_types.ReceivedRouteDetail]]` for 1st param but got
        #  `List[ctrl_types.AdvertisedRouteDetail]`.
        print_route_details(routes, key_fn, detailed)


class OriginatedRoutesCmd(PrefixMgrCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self,
        client: OpenrCtrl.Client,
    ) -> None:

        # Get data
        self.render(client.getOriginatedPrefixes())

    def render(
        self,
        originated_prefixes: List[openr_types.OriginatedPrefixEntry],
    ) -> None:
        """
        Render advertised routes
        """
        rows = [""]

        for prefix_entry in originated_prefixes:
            rows.append(f"> {prefix_entry.prefix.prefix}")
            fwd_algo = PrefixForwardingAlgorithm._VALUES_TO_NAMES.get(
                prefix_entry.prefix.forwardingAlgorithm
            )
            fwd_type = PrefixForwardingType._VALUES_TO_NAMES.get(
                prefix_entry.prefix.forwardingType
            )
            rows.append(f"     Forwarding - algorithm: {fwd_algo}, type: {fwd_type}")
            rows.append(
                f"     Metrics - path-preference: {prefix_entry.prefix.path_preference}"
                f", source-preference: {prefix_entry.prefix.source_preference}"
            )
            if prefix_entry.prefix.tags:
                # pyre-fixme[6]: Expected `Iterable[str]` for 1st param but got
                #  `Optional[typing.Set[str]]`.
                rows.append(f"     Tags - {', '.join(prefix_entry.prefix.tags)}")
            if prefix_entry.prefix.area_stack:
                rows.append(
                    # pyre-fixme[6]: Expected `Iterable[str]` for 1st param but got
                    #  `Optional[List[str]]`.
                    f"     Area Stack - {', '.join(prefix_entry.prefix.area_stack)}"
                )
            if prefix_entry.prefix.minNexthop:
                rows.append(f"     Min-nexthops: {prefix_entry.prefix.minNexthop}")
            rows.append(
                f"     Min Supporting Routes - {(prefix_entry.prefix.minimum_supporting_routes)}"
            )
            if prefix_entry.prefix.install_to_fib:
                rows.append(
                    f"     install_to_fib: {prefix_entry.prefix.install_to_fib}"
                )
            rows.append("")

        print("\n".join(rows))


class AreaAdvertisedRoutesCmd(PrefixMgrCmd):

    # @override
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self,
        client: OpenrCtrl.Client,
        area: str,
        route_filter_type: ctrl_types.RouteFilterType,
        prefixes: List[str],
        prefix_type: Optional[str],
        json: bool,
        detailed: bool,
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
        route_filter = ctrl_types.AdvertisedRouteFilter()
        if prefixes:
            route_filter.prefixes = [ipnetwork.ip_str_to_prefix(p) for p in prefixes]
        if prefix_type:
            route_filter.prefixType = self.to_thrift_prefix_type(prefix_type)

        # Get routes
        return client.getAreaAdvertisedRoutesFiltered(
            area, route_filter_type, route_filter
        )

    def render(self, routes: List[ctrl_types.AdvertisedRoute], detailed: bool) -> None:
        """
        Render advertised routes
        """

        def key_fn(key: network_types.PrefixType) -> Tuple[str]:
            return (network_types.PrefixType._VALUES_TO_NAMES.get(key, "N/A"),)

        print_advertised_routes(routes, key_fn, detailed)
