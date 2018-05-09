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
from builtins import range
from builtins import object

from openr.clients import perf_client

import tabulate


class PerfCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Perf client '''

        self.client = perf_client.PerfClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.fib_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)


class ViewFibCmd(PerfCmd):
    def run(self):
        resp = self.client.view_fib()
        headers = ['Node', 'Events', 'Duration', 'Unix Timestamp']
        for i in range(len(resp.eventInfo)):
            rows = []
            recent_ts = resp.eventInfo[i].events[0].unixTs
            total_duration = 0
            for perf_event in resp.eventInfo[i].events:
                node_name = perf_event.nodeName
                event_name = perf_event.eventDescr
                duration = perf_event.unixTs - recent_ts
                total_duration += duration
                recent_ts = perf_event.unixTs
                rows.append([node_name, event_name, duration, recent_ts])
            print('Perf Event Item: {}, total duration: {}ms'.format(i, total_duration))
            print(tabulate.tabulate(rows, headers=headers))
            print()
