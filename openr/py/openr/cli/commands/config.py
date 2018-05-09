#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import
from __future__ import print_function
from __future__ import unicode_literals
from __future__ import division
from builtins import object

import sys

from openr.utils.consts import Consts
from openr.utils.serializer import deserialize_thrift_object
from openr.cli.utils import utils
from openr.utils import ipnetwork, printing

from openr.clients import config_store_client
from openr.AllocPrefix import ttypes as ap_types
from openr.LinkMonitor import ttypes as lm_types
from openr.Lsdb import ttypes as lsdb_types


class ConfigCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Config Store client '''

        self.client = config_store_client.ConfigStoreClient(
            cli_opts.zmq_ctx,
            cli_opts.config_store_url,
            cli_opts.timeout,
            cli_opts.proto_factory)


class ConfigPrefixAllocatorCmd(ConfigCmd):
    def run(self):
        try:
            prefix_alloc_blob = self.client.load(Consts.PREFIX_ALLOC_KEY)
        except KeyError:
            print("Missing Prefix Allocator config", file=sys.stderr)
            return

        prefix_alloc = deserialize_thrift_object(
            prefix_alloc_blob, ap_types.AllocPrefix)
        self.print_config(prefix_alloc)

    def print_config(self, prefix_alloc):
        seed_prefix = prefix_alloc.seedPrefix
        seed_prefix_addr = ipnetwork.sprint_addr(seed_prefix.prefixAddress.addr)

        caption = 'Prefix Allocator parameters stored'
        rows = []
        rows.append(['Seed prefix: {}/{}'.format(seed_prefix_addr,
                    seed_prefix.prefixLength)])
        rows.append(['Allocated prefix length: {}'.format(
                    prefix_alloc.allocPrefixLen)])
        rows.append(['Allocated prefix index: {}'.format(
            prefix_alloc.allocPrefixIndex)])

        print(printing.render_vertical_table(rows, caption=caption))


class ConfigLinkMonitorCmd(ConfigCmd):
    def run(self):
        try:
            lm_config_blob = self.client.load(Consts.LINK_MONITOR_KEY)
        except KeyError:
            print("Missing Link Monitor config", file=sys.stderr)
            return

        lm_config = deserialize_thrift_object(
            lm_config_blob, lm_types.LinkMonitorConfig)
        self.print_config(lm_config)

    def print_config(self, lm_config):
        caption = 'Link Monitor parameters stored'
        rows = []
        rows.append(['isOverloaded: {}'.format(
                    'Yes' if lm_config.isOverloaded else 'No')])
        rows.append(['nodeLabel: {}'.format(lm_config.nodeLabel)])
        rows.append(['overloadedLinks: {}'.format(
            ', '.join(lm_config.overloadedLinks))])
        print(printing.render_vertical_table(rows, caption=caption))

        print(printing.render_vertical_table([['linkMetricOverrides:']]))
        column_labels = ['Interface', 'Metric Override']
        rows = []
        for (k, v) in sorted(lm_config.linkMetricOverrides.items()):
            rows.append([k, v])
        print(printing.render_horizontal_table(rows, column_labels=column_labels))

        print(printing.render_vertical_table([['adjMetricOverrides:']]))
        column_labels = ['Adjacency', 'Metric Override']
        rows = []
        for (k, v) in sorted(lm_config.adjMetricOverrides.items()):
            adj_str = k.nodeName + ' ' + k.ifName
            rows.append([adj_str, v])
        print(printing.render_horizontal_table(rows, column_labels=column_labels))


class ConfigPrefixManagerCmd(ConfigCmd):
    def run(self):
        try:
            prefix_mgr_config_blob = self.client.load(Consts.PREFIX_MGR_KEY)
        except KeyError:
            print("Missing Prefix Manager config", file=sys.stderr)
            return

        prefix_mgr_config = deserialize_thrift_object(
            prefix_mgr_config_blob, lsdb_types.PrefixDatabase)
        self.print_config(prefix_mgr_config)

    def print_config(self, prefix_mgr_config):
        print()
        print(utils.sprint_prefixes_db_full(prefix_mgr_config))
        print()


class ConfigEraseCmd(ConfigCmd):
    def run(self, key):
        success = self.client.erase(key)
        if success:
            print("Key erased\n")
        else:
            print("Erase failed\n")
            sys.exit(1)


class ConfigStoreCmd(ConfigCmd):
    def run(self, key, value):
        success = self.client.store(key, value)
        if success:
            print("Key stored\n")
        else:
            print("Store failed\n")
            sys.exit(1)
