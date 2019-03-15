#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import, division, print_function, unicode_literals

from openr.clients.openr_client import OpenrClient
from openr.Lsdb import ttypes as lsdb_types
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.PrefixManager import ttypes as prefix_mgr_types
from openr.utils import ipnetwork


class PrefixMgrClient(OpenrClient):
    def __init__(self, cli_opts):
        super(PrefixMgrClient, self).__init__(
            OpenrModuleType.PREFIX_MANAGER,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.prefix_mgr_cmd_port),
            cli_opts,
        )

    def send_cmd_to_prefix_mgr(
        self, cmd, prefixes=None, prefix_type="BREEZE", forwarding_type="IP"
    ):
        """ Send the given cmd to prefix manager and return resp """

        PREFIX_TYPE_TO_VALUES = lsdb_types.PrefixType._NAMES_TO_VALUES
        if prefix_type not in PREFIX_TYPE_TO_VALUES:
            raise Exception(
                "Unknown type {}. Use any of {}".format(
                    prefix_type, ", ".join(PREFIX_TYPE_TO_VALUES.keys())
                )
            )

        FORWARDING_TYPE_TO_VALUES = lsdb_types.PrefixForwardingType._NAMES_TO_VALUES
        if forwarding_type not in FORWARDING_TYPE_TO_VALUES:
            raise Exception(
                "Unknown forwarding type {}. Use any of {}".format(
                    forwarding_type, ", ".join(FORWARDING_TYPE_TO_VALUES.keys())
                )
            )

        req_msg = prefix_mgr_types.PrefixManagerRequest()
        req_msg.cmd = cmd
        req_msg.type = PREFIX_TYPE_TO_VALUES[prefix_type]
        req_msg.prefixes = []
        if prefixes is not None:
            for prefix in prefixes:
                req_msg.prefixes.append(
                    lsdb_types.PrefixEntry(
                        prefix=ipnetwork.ip_str_to_prefix(prefix),
                        type=PREFIX_TYPE_TO_VALUES[prefix_type],
                        forwardingType=FORWARDING_TYPE_TO_VALUES[forwarding_type],
                    )
                )

        return self.send_and_recv_thrift_obj(
            req_msg, prefix_mgr_types.PrefixManagerResponse
        )

    def add_prefix(self, prefixes, prefix_type, forwarding_type=False):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.ADD_PREFIXES,
            prefixes,
            prefix_type,
            forwarding_type,
        )

    def sync_prefix(self, prefixes, prefix_type, forwarding_type=False):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.SYNC_PREFIXES_BY_TYPE,
            prefixes,
            prefix_type,
            forwarding_type,
        )

    def withdraw_prefix(self, prefixes, prefix_type="BREEZE"):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.WITHDRAW_PREFIXES,
            prefixes,
            prefix_type,
        )

    def view_prefix(self):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.GET_ALL_PREFIXES
        )
