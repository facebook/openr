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

from openr.clients import prefix_mgr_client
from openr.utils import ipnetwork, printing
from openr.cli.utils import utils


class PrefixMgrCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Prefix Manager client '''

        self.client = prefix_mgr_client.PrefixMgrClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.prefix_mgr_cmd_port),
            cli_opts.timeout,
            cli_opts.proto_factory)


class WithdrawCmd(PrefixMgrCmd):
    def run(self, prefixes):
        resp = self.client.withdraw_prefix(prefixes)

        if (not resp.success):
            print("Could not withdraw. Error {}".format(resp.message))
        else:
            print("Withdrew {} prefixes".format(len(prefixes)))


class AdvertiseCmd(PrefixMgrCmd):
    def run(self, prefixes, prefix_type):
        resp = self.client.add_prefix(prefixes, prefix_type)

        if (not resp.success):
            print("Could not advertise. Error {}".format(resp.message))
        else:
            print("Advertised {} prefixes with type {}".format(len(prefixes),
                                                               prefix_type))


class SyncCmd(PrefixMgrCmd):
    def run(self, prefixes, prefix_type):
        resp = self.client.sync_prefix(prefixes, prefix_type)

        if (not resp.success):
            print("Could not sync prefixes. Error {}".format(resp.message))
        else:
            print("Synced {} prefixes with type {}".format(len(prefixes),
                                                           prefix_type))


class ViewCmd(PrefixMgrCmd):
    def run(self):
        resp = self.client.view_prefix()
        rows = []
        for prefix_entry in resp.prefixes:
            prefix_str = ipnetwork.sprint_prefix(prefix_entry.prefix)
            prefix_type = ipnetwork.sprint_prefix_type(prefix_entry.type)
            rows.append((prefix_type, prefix_str))
        print('\n', printing.render_horizontal_table(rows, ['Type', 'Prefix']))
        print()
