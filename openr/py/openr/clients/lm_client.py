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

from openr.LinkMonitor import ttypes as lm_types
from openr.utils import socket, consts

import zmq


class LMClient(object):
    def __init__(self, zmq_ctx, lm_cmd_url, timeout=consts.Consts.TIMEOUT_MS,
                 proto_factory=consts.Consts.PROTO_FACTORY):
        self._lm_cmd_socket = socket.Socket(zmq_ctx, zmq.DEALER, timeout, proto_factory)
        self._lm_cmd_socket.connect(lm_cmd_url)

    def dump_links(self, all=True):
        '''
        @param all: If set to false then links with no addresses will be
                    filtered out.
        '''

        req_msg = lm_types.LinkMonitorRequest()
        req_msg.cmd = lm_types.LinkMonitorCommand.DUMP_LINKS
        req_msg.interfaceName = ''

        self._lm_cmd_socket.send_thrift_obj(req_msg)
        links = self._lm_cmd_socket.recv_thrift_obj(lm_types.DumpLinksReply)

        # filter out link with no addresses
        if not all:
            links.interfaceDetails = {
                k: v for k, v in links.interfaceDetails.items()
                if len(v.info.networks) != 0}

        return links

    def get_identity(self):

        return self.dump_links().thisNodeName

    def send_link_monitor_cmd(self, command, interface='', metric=0, node=''):

        req_msg = lm_types.LinkMonitorRequest(command, interface, metric, node)
        self._lm_cmd_socket.send_thrift_obj(req_msg)

        return self.dump_links()

    def set_unset_overload(self, overload):

        SET = lm_types.LinkMonitorCommand.SET_OVERLOAD
        UNSET = lm_types.LinkMonitorCommand.UNSET_OVERLOAD

        return self.send_link_monitor_cmd(SET if overload else UNSET)

    def set_unset_link_overload(self, overload, interface):

        SET = lm_types.LinkMonitorCommand.SET_LINK_OVERLOAD
        UNSET = lm_types.LinkMonitorCommand.UNSET_LINK_OVERLOAD

        return self.send_link_monitor_cmd(SET if overload else UNSET, interface)

    def set_unset_link_metric(self, override, interface, metric):

        SET = lm_types.LinkMonitorCommand.SET_LINK_METRIC
        UNSET = lm_types.LinkMonitorCommand.UNSET_LINK_METRIC

        return self.send_link_monitor_cmd(SET if override else UNSET, interface, metric)

    def set_unset_adj_metric(self, override, node, interface, metric):

        SET = lm_types.LinkMonitorCommand.SET_ADJ_METRIC
        UNSET = lm_types.LinkMonitorCommand.UNSET_ADJ_METRIC

        return self.send_link_monitor_cmd(SET if override else UNSET,
                                                  interface, metric, node)

    def get_openr_version(self):

        command = lm_types.LinkMonitorCommand.GET_VERSION

        req_msg = lm_types.LinkMonitorRequest(command)
        self._lm_cmd_socket.send_thrift_obj(req_msg)

        return \
           self._lm_cmd_socket.recv_thrift_obj(lm_types.OpenrVersions)

    def get_build_info(self):

        command = lm_types.LinkMonitorCommand.GET_BUILD_INFO
        req_msg = lm_types.LinkMonitorRequest(command)
        self._lm_cmd_socket.send_thrift_obj(req_msg)

        return self._lm_cmd_socket.recv_thrift_obj(lm_types.BuildInfo)
