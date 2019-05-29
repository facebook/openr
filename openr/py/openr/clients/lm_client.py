#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from openr.clients.openr_client import OpenrClientDeprecated
from openr.LinkMonitor import ttypes as lm_types
from openr.OpenrCtrl.ttypes import OpenrModuleType


class LMClient(OpenrClientDeprecated):
    def __init__(self, cli_opts):
        super(LMClient, self).__init__(
            OpenrModuleType.LINK_MONITOR,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.lm_cmd_port),
            cli_opts,
        )

    def dump_links(self, all=True):
        """
        @param all: If set to false then links with no addresses will be
                    filtered out.
        """

        req_msg = lm_types.LinkMonitorRequest()
        req_msg.cmd = lm_types.LinkMonitorCommand.DUMP_LINKS

        links = self.send_and_recv_thrift_obj(req_msg, lm_types.DumpLinksReply)

        # filter out link with no addresses
        if not all:
            links.interfaceDetails = {
                k: v
                for k, v in links.interfaceDetails.items()
                if len(v.info.networks) != 0
            }

        return links

    def get_identity(self):

        return self.dump_links().thisNodeName

    def get_build_info(self):
        return self.send_and_recv_thrift_obj(
            lm_types.LinkMonitorRequest(lm_types.LinkMonitorCommand.GET_BUILD_INFO),
            lm_types.BuildInfo,
        )
