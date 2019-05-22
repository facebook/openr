#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from typing import List

from openr.cli.utils.commands import OpenrCtrlCmd
from openr.Lsdb import ttypes as lsdb_types
from openr.Network import ttypes as network_types
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import ipnetwork, printing


class PrefixMgrCmd(OpenrCtrlCmd):
    def to_thrift_prefixes(
        self,
        prefixes: List[str],
        prefix_type: network_types.PrefixType,
        forwarding_type: lsdb_types.PrefixForwardingType = lsdb_types.PrefixForwardingType.IP,
    ) -> List[lsdb_types.PrefixEntry]:
        return [
            lsdb_types.PrefixEntry(
                prefix=ipnetwork.ip_str_to_prefix(prefix),
                type=prefix_type,
                forwardingType=forwarding_type,
            )
            for prefix in prefixes
        ]

    def to_thrift_prefix_type(self, prefix_type: str) -> network_types.PrefixType:
        PREFIX_TYPE_TO_VALUES = network_types.PrefixType._NAMES_TO_VALUES
        if prefix_type not in PREFIX_TYPE_TO_VALUES:
            raise Exception(
                "Unknown type {}. Use any of {}".format(
                    prefix_type, ", ".join(PREFIX_TYPE_TO_VALUES.keys())
                )
            )

        return PREFIX_TYPE_TO_VALUES[prefix_type]

    def to_thrift_forwarding_type(
        self, forwarding_type: str
    ) -> lsdb_types.PrefixForwardingType:
        FORWARDING_TYPE_TO_VALUES = lsdb_types.PrefixForwardingType._NAMES_TO_VALUES
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
            is_ephemeral_str = ipnetwork.sprint_prefix_is_ephemeral(prefix_entry)
            rows.append((prefix_type, prefix_str, forwarding_type, is_ephemeral_str))
        print(
            "\n",
            printing.render_horizontal_table(
                rows, ["Type", "Prefix", "Forwarding Type", "Ephemeral"]
            ),
        )
        print()
