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

import tabulate
import json
import time
import os
import zmq

from openr.cli.utils import utils
from openr.utils import consts, printing, socket
from openr.clients import monitor_client, monitor_subscriber
from fbzmq.Monitor import ttypes as monitor_types


class MonitorCmd(object):
    def __init__(self, cli_opts):
        ''' initialize the Monitor client '''

        self.host = cli_opts.host
        self.lm_cmd_port = cli_opts.lm_cmd_port
        self.cli_opts = cli_opts
        self.monitor_pub_port = cli_opts.monitor_pub_port

        self.client = monitor_client.MonitorClient(
            cli_opts.zmq_ctx,
            "tcp://[{}]:{}".format(cli_opts.host, cli_opts.monitor_rep_port),
            cli_opts.timeout,
            cli_opts.proto_factory)


class CountersCmd(MonitorCmd):
    def run(self, prefix='', is_json=False):

        resp = self.client.dump_all_counter_data()
        self.print_counters(resp, prefix, is_json)

    def print_counters(self, resp, prefix, is_json):
        ''' print the Kv Store counters '''

        host_id = utils.get_connected_node_name(self.host, self.lm_cmd_port)
        caption = '{}\'s counters'.format(host_id)

        rows = []
        for key, counter in sorted(resp.counters.items()):
            if not key.startswith(prefix):
                continue
            rows.append([key, ':', counter.value])

        if is_json:
            json_data = {k: v for k, _, v in rows}
            print(utils.json_dumps(json_data))
        else:
            print(printing.render_horizontal_table(rows, caption=caption,
                                                   tablefmt='plain'))
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


def print_log_list_type(llist):

    idx = 1
    str_txt = '{}'.format(''.join(llist[0]) + '\n')

    while idx < len(llist):
        str_txt += '{:<18} {}'.format('', ''.join(llist[idx]) + '\n')
        idx += 1

    return str_txt


def print_log_sample(log_sample):

    columns = ['', '']
    rows = []
    for _, value0 in log_sample.items():
        for key, value in value0.items():

            if key == "time":
                value = (time.strftime('%H:%M:%S %Y-%m-%d',
                            time.localtime(value)))
            if type(value) is list:
                value = print_log_list_type(value)

            rows.append(['{:<17}  {}'.format(key, value)])

    print(tabulate.tabulate(rows, headers=columns, tablefmt='plain'))


class SnoopCmd(MonitorCmd):

    counters_db = {}

    def run(self, log, counters, delta, duration):

        pub_client = monitor_subscriber.MonitorSubscriber(
            zmq.Context(),
            "tcp://[{}]:{}".format(self.host, self.monitor_pub_port),
            timeout=1000)

        start_time = time.time()
        while True:
            # End loop if it is time!
            if duration > 0 and time.time() - start_time > duration:
                break

            # we do not want to timeout. keep listening for a change
            try:
                msg = pub_client.listen()
                self.print_snoop_data(log, counters, delta, msg)
            except zmq.error.Again:
                pass

    def print_counter_data(self, delta, counters_data):
        columns = ['{:<45}'.format('Key'), 'Previous Value',
                    'Value', 'TimeStamp']
        rows = []

        for key, value in counters_data.counters.items():
            if delta and key in self.counters_db:
                if value.value == self.counters_db[key]:
                    continue

            prev_val = self.counters_db[key] if key in self.counters_db else 0
            self.counters_db[key] = value.value
            rows.append([key, prev_val, '=> {}'.format(value.value),
                            '(ts={})'.format(value.timestamp)])

        if rows:
            print(printing.render_horizontal_table(rows, columns))

    def print_event_log_data(self, event_log_data):
        log_sample = json.loads(' '.join(event_log_data.samples))
        print_log_sample(log_sample)

    def print_snoop_data(self, log, counters, delta, resp):
        ''' print the snoop data'''

        if resp.pubType == monitor_types.PubType.COUNTER_PUB and counters:
            self.print_counter_data(delta, resp.counterPub)
        elif resp.pubType == monitor_types.PubType.EVENT_LOG_PUB and log:
            self.print_event_log_data(resp.eventLogPub)


class LogCmd(MonitorCmd):
    def run(self, json_opt=False):

        resp = self.client.dump_log_data()
        self.print_log_data(resp, json_opt)

    def print_log_data(self, resp, json_opt):
        ''' print the log data'''

        def update_func(json_obj, thrift_obj):
            json_obj['eventLogs'] = \
                [utils.thrift_to_dict(e) for e in thrift_obj.eventLogs]

        if json_opt:
            data = utils.thrift_to_dict(resp, update_func)
            print(utils.json_dumps(data))
            return

        log_samples = []
        for event_log in resp.eventLogs:
            log_samples.extend([json.loads(lg) for lg in event_log.samples])

        for log_sample in log_samples:
            print_log_sample(log_sample)


class StatisticsCmd(MonitorCmd):

    def run(self):
        stats_templates = [
            {
                'title': 'KvStore Stats',
                'counters': [
                    ('KeyVals', 'kvstore.num_keys'),
                    ('Peering Sessions', 'kvstore.num_peers'),
                    ('Pending Sync', 'kvstore.pending_full_sync'),
                ],
                'stats': [
                    (
                        'Rcvd Publications',
                        'kvstore.received_publications.count',
                    ),
                    ('Rcvd KeyVals', 'kvstore.received_key_vals.sum'),
                    ('Updates KeyVals', 'kvstore.updated_key_vals.sum'),
                ],
            },
            {
                'title': 'LinkMonitor Stats',
                'counters': [
                    ('Adjacent Neighbors', 'spark.num_adjacent_neighbors'),
                    ('Tracked Neighbors', 'spark.num_tracked_neighbors'),
                ],
                'stats': [
                    ('Updates AdjDb', 'link_monitor.advertise_adjacencies.sum'),
                    ('Rcvd Hello Pkts', 'spark.hello_packet_recv.sum'),
                    ('Sent Hello Pkts', 'spark.hello_packet_sent.sum'),
                ],
            },
            {
                'title': 'Decision/Fib Stats',
                'counters': [
                ],
                'stats': [
                    ('Updates AdjDbs', 'decision.adj_db_update.count'),
                    ('Updates PrefixDbs', 'decision.prefix_db_update.count'),
                    ('SPF Runs', 'decision.spf_runs.count'),
                    ('SPF Avg Duration (ms)', 'decision.spf_duration.avg'),
                    (
                        'Convergence Duration (ms)',
                        'fib.convergence_time_ms.avg',
                    ),
                    ('Updates RouteDb', 'fib.process_route_db.count'),
                    ('Full Route Sync', 'fib.sync_fib_calls.count'),
                ],
            },
        ]

        counters = self.client.dump_all_counter_data().counters
        self.print_stats(stats_templates, counters)

    def print_stats(self, stats_templates, counters):
        '''
        Print in pretty format
        '''

        suffixes = ['60', '600', '3600', '0']

        for template in stats_templates:
            counters_rows = []
            for title, key in template['counters']:
                val = counters.get(key, None)
                counters_rows.append([title, 'N/A' if not val else val.value])

            stats_cols = ['Stat', '1 min', '10 mins', '1 hour', 'All Time']
            stats_rows = []
            for title, key_prefix in template['stats']:
                row = [title]
                for key in ['{}.{}'.format(key_prefix, s) for s in suffixes]:
                    val = counters.get(key, None)
                    row.append('N/A' if not val else val.value)
                stats_rows.append(row)

            print('> {} '.format(template['title']))
            if counters_rows:
                print()
                print(printing.render_horizontal_table(
                    counters_rows,
                    tablefmt='plain',
                ).strip('\n'))
            if stats_rows:
                print()
                print(printing.render_horizontal_table(
                    stats_rows,
                    column_labels=stats_cols,
                    tablefmt='simple',
                ).strip('\n'))
