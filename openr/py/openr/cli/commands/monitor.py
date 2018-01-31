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

import os
import zmq

from openr.cli.utils import utils
from openr.clients import monitor_client
from openr.utils import consts, printing, socket


class MonitorCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Monitor client '''

        self.host = cli_opts.host
        self.lm_cmd_port = cli_opts.lm_cmd_port
        self.cli_opts = cli_opts

        self.client = monitor_client.MonitorClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.monitor_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)


class CountersCmd(MonitorCmd):
    def run(self, prefix=''):

        resp = self.client.dump_all_counter_data()
        self.print_counters(resp, prefix)

    def print_counters(self, resp, prefix):
        ''' print the Kv Store counters '''

        host_id = utils.get_connected_node_name(self.host, self.lm_cmd_port)
        caption = '{}\'s counters'.format(host_id)

        rows = []
        for key, counter in sorted(resp.counters.items()):
            if not key.startswith(prefix):
                continue
            rows.append(['{} : {}'.format(key, counter.value)])

        print(printing.render_horizontal_table(rows, caption=caption, tablefmt='plain'))
        print()


class ForceCrashCmd(MonitorCmd):
    def run(self, yes):

        if not yes:
            yes = utils.yesno('Are you sure to trigger Open/R crash')

        if not yes:
            print('Not triggering force crash')
            return

        print('Triggering force crash')
        sock = socket.Socket(self.cli_opts.zmq_ctx, zmq.REQ, timeout=200)
        sock.set_sock_opt(zmq.LINGER, 1000)
        sock.connect(consts.Consts.FORCE_CRASH_SERVER_URL)
        sock.send('User {} issuing crash command'.format(os.environ['USER']))
        sock.close()
