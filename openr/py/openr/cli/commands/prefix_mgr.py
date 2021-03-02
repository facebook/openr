#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from typing import List, Optional, Tuple

from openr.cli.utils.commands import OpenrCtrlCmd
from openr.cli.utils.utils import print_route_details
from openr.Network import ttypes as network_types
from openr.OpenrConfig.ttypes import PrefixForwardingType
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
    def _run(
        self, client: OpenrCtrl.Client, prefixes: List[str], prefix_type: str
    ) -> None:
        tprefixes = self.to_thrift_prefixes(
            prefixes, self.to_thrift_prefix_type(prefix_type)
        )
        client.withdrawPrefixes(tprefixes)
        print(f"Withdrew {len(prefixes)} prefixes")


class AdvertiseCmd(PrefixMgrCmd):
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

        print_route_details(routes, key_fn, detailed)
