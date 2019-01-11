#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import, division, print_function, unicode_literals

from builtins import object

import zmq
from openr.clients.openr_client import OpenrClient
from openr.Lsdb import ttypes as lsdb_types
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.PrefixManager import ttypes as prefix_mgr_types
from openr.utils import consts, ipnetwork, zmq_socket


class PrefixMgrClient(OpenrClient):
    def __init__(self, cli_opts):
        super(PrefixMgrClient, self).__init__(
            OpenrModuleType.PREFIX_MANAGER,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.prefix_mgr_cmd_port),
            cli_opts,
        )

    def send_cmd_to_prefix_mgr(self, cmd, prefixes=None, prefix_type="BREEZE"):
        """ Send the given cmd to prefix manager and return resp """

        TYPE_TO_VALUES = lsdb_types.PrefixType._NAMES_TO_VALUES
        if prefix_type not in TYPE_TO_VALUES:
            raise Exception(
                "Unknown type {}. Use any of {}".format(
                    prefix_type, ", ".join(TYPE_TO_VALUES.keys())
                )
            )

        req_msg = prefix_mgr_types.PrefixManagerRequest()
        req_msg.cmd = cmd
        req_msg.type = TYPE_TO_VALUES[prefix_type]
        req_msg.prefixes = []
        if prefixes is not None:
            for prefix in prefixes:
                req_msg.prefixes.append(
                    lsdb_types.PrefixEntry(
                        prefix=ipnetwork.ip_str_to_prefix(prefix),
                        type=TYPE_TO_VALUES[prefix_type],
                    )
                )

        return self.send_and_recv_thrift_obj(
            req_msg, prefix_mgr_types.PrefixManagerResponse
        )

    def add_prefix(self, prefixes, prefix_type):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.ADD_PREFIXES, prefixes, prefix_type
        )

    def sync_prefix(self, prefixes, prefix_type):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.SYNC_PREFIXES_BY_TYPE,
            prefixes,
            prefix_type,
        )

    def withdraw_prefix(self, prefixes):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.WITHDRAW_PREFIXES, prefixes
        )

    def view_prefix(self):
        return self.send_cmd_to_prefix_mgr(
            prefix_mgr_types.PrefixManagerCommand.GET_ALL_PREFIXES
        )
