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

from openr.cli.utils import utils
from openr.clients import monitor_client
from openr.utils import printing


class MonitorCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Monitor client '''

        self.host = cli_opts.host
        self.lm_cmd_port = cli_opts.lm_cmd_port

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
