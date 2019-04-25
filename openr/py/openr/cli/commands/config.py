#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


from openr.AllocPrefix import ttypes as ap_types
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.LinkMonitor import ttypes as lm_types
from openr.Lsdb import ttypes as lsdb_types
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import ipnetwork, printing
from openr.utils.consts import Consts
from openr.utils.serializer import deserialize_thrift_object


class ConfigPrefixAllocatorCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client):
        prefix_alloc_blob = client.getConfigKey(Consts.PREFIX_ALLOC_KEY)
        prefix_alloc = deserialize_thrift_object(
            prefix_alloc_blob, ap_types.AllocPrefix
        )
        self.print_config(prefix_alloc)

    def print_config(self, prefix_alloc: ap_types.AllocPrefix) -> None:
        seed_prefix = prefix_alloc.seedPrefix
        seed_prefix_addr = ipnetwork.sprint_addr(seed_prefix.prefixAddress.addr)

        caption = "Prefix Allocator parameters stored"
        rows = []
        rows.append(
            ["Seed prefix: {}/{}".format(seed_prefix_addr, seed_prefix.prefixLength)]
        )
        rows.append(["Allocated prefix length: {}".format(prefix_alloc.allocPrefixLen)])
        rows.append(
            ["Allocated prefix index: {}".format(prefix_alloc.allocPrefixIndex)]
        )

        print(printing.render_vertical_table(rows, caption=caption))


class ConfigLinkMonitorCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client) -> None:
        lm_config_blob = client.getConfigKey(Consts.LINK_MONITOR_KEY)
        lm_config = deserialize_thrift_object(
            lm_config_blob, lm_types.LinkMonitorConfig
        )
        self.print_config(lm_config)

    def print_config(self, lm_config: lm_types.LinkMonitorConfig):
        caption = "Link Monitor parameters stored"
        rows = []
        rows.append(
            ["isOverloaded: {}".format("Yes" if lm_config.isOverloaded else "No")]
        )
        rows.append(["nodeLabel: {}".format(lm_config.nodeLabel)])
        rows.append(
            ["overloadedLinks: {}".format(", ".join(lm_config.overloadedLinks))]
        )
        print(printing.render_vertical_table(rows, caption=caption))

        print(printing.render_vertical_table([["linkMetricOverrides:"]]))
        column_labels = ["Interface", "Metric Override"]
        rows = []
        for (k, v) in sorted(lm_config.linkMetricOverrides.items()):
            rows.append([k, v])
        print(printing.render_horizontal_table(rows, column_labels=column_labels))

        print(printing.render_vertical_table([["adjMetricOverrides:"]]))
        column_labels = ["Adjacency", "Metric Override"]
        rows = []
        for (k, v) in sorted(lm_config.adjMetricOverrides.items()):
            adj_str = k.nodeName + " " + k.ifName
            rows.append([adj_str, v])
        print(printing.render_horizontal_table(rows, column_labels=column_labels))


class ConfigPrefixManagerCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client) -> None:
        prefix_mgr_config_blob = client.getConfigKey(Consts.PREFIX_MGR_KEY)
        prefix_mgr_config = deserialize_thrift_object(
            prefix_mgr_config_blob, lsdb_types.PrefixDatabase
        )
        self.print_config(prefix_mgr_config)

    def print_config(self, prefix_mgr_config: lsdb_types.PrefixDatabase):
        print()
        print(utils.sprint_prefixes_db_full(prefix_mgr_config))
        print()


class ConfigEraseCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client, key: str) -> None:
        client.eraseConfigKey(key)
        print("Key erased")


class ConfigStoreCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client, key: str, value: str) -> None:
        client.setConfigKey(key, value)
        print("Key-Value stored")
