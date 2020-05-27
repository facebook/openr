#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import json
from typing import Tuple

import click
import jsondiff
from openr.AllocPrefix import ttypes as ap_types
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.LinkMonitor import ttypes as lm_types
from openr.Lsdb import ttypes as lsdb_types
from openr.OpenrCtrl import OpenrCtrl
from openr.OpenrCtrl.ttypes import OpenrError
from openr.utils import ipnetwork, printing
from openr.utils.consts import Consts
from openr.utils.serializer import deserialize_thrift_object


class ConfigShowCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client):
        resp = client.getRunningConfig()
        config = json.loads(resp)
        print(json.dumps(config, indent=4, sort_keys=True, separators=(",", ": ")))


class ConfigDryRunCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client, file: str):
        try:
            client.dryrunConfig(file)
            click.echo(click.style("SUCCESS", fg="green"))
        except OpenrError as ex:
            click.echo(click.style("FAILED: {}".format(ex), fg="red"))


class ConfigCompareCmd(OpenrCtrlCmd):
    def _run(self, client: OpenrCtrl.Client, file: str):
        running_conf = client.getRunningConfig()

        try:
            file_conf = client.dryrunConfig(file)
        except OpenrError as ex:
            print("invalid config {} : {}".format(file, ex))
            return

        res = jsondiff.diff(running_conf, file_conf, load=True, syntax="explicit")
        if res:
            click.echo(click.style("DIFF FOUND!", fg="red"))
            print("== diff(running_conf, {}) ==".format(file))
            print(res)
        else:
            click.echo(click.style("SAME", fg="green"))


class ConfigStoreCmdBase(OpenrCtrlCmd):
    def getConfigWrapper(
        self, client: OpenrCtrl.Client, config_key: str
    ) -> Tuple[str, str]:
        blob = None
        exception_str = None
        try:
            blob = client.getConfigKey(config_key)
        except OpenrError as ex:
            exception_str = "Exception getting key for {}: {}".format(config_key, ex)

        return (blob, exception_str)


class ConfigPrefixAllocatorCmd(ConfigStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client):
        (prefix_alloc_blob, exception_str) = self.getConfigWrapper(
            client, Consts.PREFIX_ALLOC_KEY
        )

        if prefix_alloc_blob is None:
            print(exception_str)
            return

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


class ConfigLinkMonitorCmd(ConfigStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        # After link-monitor thread starts, it will hold for
        # "adjHoldUntilTimePoint_" time before populate config information.
        # During this short time-period, Exception can be hit if dump cmd
        # kicks during this time period.
        (lm_config_blob, exception_str) = self.getConfigWrapper(
            client, Consts.LINK_MONITOR_KEY
        )

        if lm_config_blob is None:
            print(exception_str)
            return

        lm_config = deserialize_thrift_object(lm_config_blob, lm_types.LinkMonitorState)
        self.print_config(lm_config)

    def print_config(self, lm_config: lm_types.LinkMonitorState):
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


class ConfigPrefixManagerCmd(ConfigStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client) -> None:
        (prefix_mgr_config_blob, exception_str) = self.getConfigWrapper(
            client, Consts.PREFIX_MGR_KEY
        )

        if prefix_mgr_config_blob is None:
            print(exception_str)
            return

        prefix_mgr_config = deserialize_thrift_object(
            prefix_mgr_config_blob, lsdb_types.PrefixDatabase
        )
        self.print_config(prefix_mgr_config)

    def print_config(self, prefix_mgr_config: lsdb_types.PrefixDatabase):
        print()
        print(utils.sprint_prefixes_db_full(prefix_mgr_config))
        print()


class ConfigEraseCmd(ConfigStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, key: str) -> None:
        client.eraseConfigKey(key)
        print("Key:{} erased".format(key))


class ConfigStoreCmd(ConfigStoreCmdBase):
    def _run(self, client: OpenrCtrl.Client, key: str, value: str) -> None:
        client.setConfigKey(key, value)
        print("Key:{}, value:{} stored".format(key, value))
