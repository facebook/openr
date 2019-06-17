#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import json
import os
import time
from builtins import object
from typing import Dict, List

import tabulate
import zmq
from fbzmq.Monitor import ttypes as monitor_types
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.clients import monitor_subscriber
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import consts, printing, zmq_socket


class MonitorCmd(OpenrCtrlCmd):
    def print_log_list_type(self, llist: List) -> str:
        idx = 1
        str_txt = "{}".format("".join(llist[0]) + "\n")
        while idx < len(llist):
            str_txt += "{:<18} {}".format("", "".join(llist[idx]) + "\n")
            idx += 1

        return str_txt

    def print_log_sample(self, log_sample: Dict) -> None:
        columns = ["", ""]
        rows = []
        for _, value0 in log_sample.items():
            for key, value in value0.items():
                if key == "time":
                    value = time.strftime("%H:%M:%S %Y-%m-%d", time.localtime(value))
                if type(value) is list:
                    value = self.print_log_list_type(value)

                rows.append(["{:<17}  {}".format(key, value)])

        print(tabulate.tabulate(rows, headers=columns, tablefmt="plain"))


class CountersCmd(MonitorCmd):
    def _run(
        self, client: OpenrCtrl.Client, prefix: str = "", json: bool = False
    ) -> None:
        resp = client.getCounters()
        self.print_counters(resp, prefix, json)

    def print_counters(self, resp: Dict, prefix: str, json: bool) -> None:
        """ print the Kv Store counters """

        host_id = utils.get_connected_node_name(self.cli_opts)
        caption = "{}'s counters".format(host_id)

        rows = []
        for key, counter in sorted(resp.items()):
            if not key.startswith(prefix):
                continue
            rows.append([key, ":", counter])

        if json:
            json_data = {k: v for k, _, v in rows}
            print(utils.json_dumps(json_data))
        else:
            print(
                printing.render_horizontal_table(
                    rows, caption=caption, tablefmt="plain"
                )
            )
            print()


class ForceCrashCmd(MonitorCmd):
    def _run(self, client: OpenrCtrl.Client, yes: bool):

        if not yes:
            yes = utils.yesno("Are you sure to trigger Open/R crash")

        if not yes:
            print("Not triggering force crash")
            return

        print("Triggering force crash")
        sock = zmq_socket.ZmqSocket(self.cli_opts.zmq_ctx, zmq.REQ, timeout=200)
        sock.set_sock_opt(zmq.LINGER, 1000)
        sock.connect(consts.Consts.FORCE_CRASH_SERVER_URL)
        sock.send(
            bytes("User {} issuing crash command".format(os.environ["USER"]), "utf-8")
        )
        sock.close()


class SnoopCmd(MonitorCmd):

    counters_db = {}

    def _run(
        self,
        client: OpenrCtrl.Client,
        log: True,
        counters: True,
        delta: True,
        duration: int,
    ) -> None:

        pub_client = monitor_subscriber.MonitorSubscriber(
            zmq.Context(),
            "tcp://[{}]:{}".format(self.host, self.monitor_pub_port),
            timeout=1000,
        )

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
        columns = ["{:<45}".format("Key"), "Previous Value", "Value", "TimeStamp"]
        rows = []

        for key, value in counters_data.counters.items():
            if delta and key in self.counters_db:
                if value.value == self.counters_db[key]:
                    continue

            prev_val = self.counters_db[key] if key in self.counters_db else 0
            self.counters_db[key] = value.value
            rows.append(
                [
                    key,
                    prev_val,
                    "=> {}".format(value.value),
                    "(ts={})".format(value.timestamp),
                ]
            )

        if rows:
            print(printing.render_horizontal_table(rows, columns))

    def print_event_log_data(self, event_log_data):
        log_sample = json.loads(" ".join(event_log_data.samples))
        self.print_log_sample(log_sample)

    def print_snoop_data(self, log, counters, delta, resp):
        """ print the snoop data"""

        if resp.pubType == monitor_types.PubType.COUNTER_PUB and counters:
            self.print_counter_data(delta, resp.counterPub)
        elif resp.pubType == monitor_types.PubType.EVENT_LOG_PUB and log:
            self.print_event_log_data(resp.eventLogPub)


class LogCmd(MonitorCmd):
    def _run(self, client: OpenrCtrl.Client, json_opt: bool = False) -> None:
        resp = client.getEventLogs()
        self.print_log_data(resp, json_opt)

    def print_log_data(self, resp, json_opt):
        """ print the log data"""

        if json_opt:
            data = {}
            data["eventLogs"] = [utils.thrift_to_dict(e) for e in resp]
            print(utils.json_dumps(data))

        else:
            log_samples = []
            for event_log in resp:
                log_samples.extend([json.loads(lg) for lg in event_log.samples])

            for log_sample in log_samples:
                self.print_log_sample(log_sample)


class StatisticsCmd(MonitorCmd):
    def _run(self, client: OpenrCtrl.Client) -> None:
        stats_templates = [
            {
                "title": "KvStore Stats",
                "counters": [
                    ("KeyVals", "kvstore.num_keys"),
                    ("Peering Sessions", "kvstore.num_peers"),
                    ("Pending Sync", "kvstore.pending_full_sync"),
                ],
                "stats": [
                    ("Rcvd Publications", "kvstore.received_publications.count"),
                    ("Rcvd KeyVals", "kvstore.received_key_vals.sum"),
                    ("Updates KeyVals", "kvstore.updated_key_vals.sum"),
                ],
            },
            {
                "title": "LinkMonitor Stats",
                "counters": [
                    ("Adjacent Neighbors", "spark.num_adjacent_neighbors"),
                    ("Tracked Neighbors", "spark.num_tracked_neighbors"),
                ],
                "stats": [
                    ("Updates AdjDb", "link_monitor.advertise_adjacencies.sum"),
                    ("Rcvd Hello Pkts", "spark.hello_packet_recv.sum"),
                    ("Sent Hello Pkts", "spark.hello_packet_sent.sum"),
                ],
            },
            {
                "title": "Decision/Fib Stats",
                "counters": [],
                "stats": [
                    ("Updates AdjDbs", "decision.adj_db_update.count"),
                    ("Updates PrefixDbs", "decision.prefix_db_update.count"),
                    ("SPF Runs", "decision.spf_runs.count"),
                    ("SPF Avg Duration (ms)", "decision.spf_duration.avg"),
                    ("Convergence Duration (ms)", "fib.convergence_time_ms.avg"),
                    ("Updates RouteDb", "fib.process_route_db.count"),
                    ("Full Route Sync", "fib.sync_fib_calls.count"),
                ],
            },
        ]

        counters = client.getCounters()
        self.print_stats(stats_templates, counters)

    def print_stats(self, stats_templates, counters):
        """
        Print in pretty format
        """

        suffixes = ["60", "600", "3600", "0"]

        for template in stats_templates:
            counters_rows = []
            for title, key in template["counters"]:
                val = counters.get(key, None)
                counters_rows.append([title, "N/A" if not val else val])

            stats_cols = ["Stat", "1 min", "10 mins", "1 hour", "All Time"]
            stats_rows = []
            for title, key_prefix in template["stats"]:
                row = [title]
                for key in ["{}.{}".format(key_prefix, s) for s in suffixes]:
                    val = counters.get(key, None)
                    row.append("N/A" if not val else val)
                stats_rows.append(row)

            print("> {} ".format(template["title"]))
            if counters_rows:
                print()
                print(
                    printing.render_horizontal_table(
                        counters_rows, tablefmt="plain"
                    ).strip("\n")
                )
            if stats_rows:
                print()
                print(
                    printing.render_horizontal_table(
                        stats_rows, column_labels=stats_cols, tablefmt="simple"
                    ).strip("\n")
                )
