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

from fbzmq.Monitor import ttypes as monitor_types
from openr.utils import socket, consts

import zmq


class MonitorClient(object):
    def __init__(self, zmq_ctx, monitor_cmd_url, timeout=consts.Consts.TIMEOUT_MS,
                 proto_factory=consts.Consts.PROTO_FACTORY):
        self._monitor_cmd_socket = socket.Socket(zmq_ctx, zmq.DEALER, timeout,
                                                  proto_factory)
        self._monitor_cmd_socket.connect(monitor_cmd_url)

    def dump_all_counter_data(self):

        request = monitor_types.MonitorRequest()
        request.cmd = monitor_types.MonitorCommand.DUMP_ALL_COUNTER_DATA

        self._monitor_cmd_socket.send_thrift_obj(request)
        return self._monitor_cmd_socket.recv_thrift_obj(
            monitor_types.CounterValuesResponse)

    def dump_log_data(self):

        request = monitor_types.MonitorRequest()
        request.cmd = monitor_types.MonitorCommand.GET_EVENT_LOGS

        self._monitor_cmd_socket.send_thrift_obj(request)

        return self._monitor_cmd_socket.recv_thrift_obj(
            monitor_types.EventLogsResponse)
