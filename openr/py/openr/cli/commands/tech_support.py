#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import os
import subprocess
import sys
from builtins import object

from openr.cli.commands import (
    config,
    decision,
    fib,
    kvstore,
    lm,
    monitor,
    openr,
    perf,
    prefix_mgr,
)
from openr.cli.utils.utils import parse_nodes
from openr.utils.consts import Consts


class TechSupportCmd(object):
    def __init__(self, cli_opts):
        """initialize the tech support command"""
        self.cli_opts = cli_opts
        # Keep short timeout
        self.cli_opts.timeout = 1000
        # Print routes or not
        self.print_routes = False

    def run(self, routes):
        self.print_routes = routes
        funcs = [
            ("openr config file", self.print_config_file),
            ("openr runtime params", self.print_runtime_params),
            ("openr version", self.print_openr_version),
            ("openr config", self.print_config),
            ("breeze lm links", self.print_lm_links),
            ("breeze kvstore peers", self.print_kvstore_peers),
            ("breeze kvstore nodes", self.print_kvstore_nodes),
            ("breeze kvstore prefixes", self.print_kvstore_prefixes),
            ("breeze kvstore keys --ttl", self.print_kvstore_keys),
            ("breeze decision adj", self.print_decision_adjs),
            ("breeze decision validate", self.print_decision_validate),
            ("breeze decision routes", self.print_decision_routes),
            ("breeze fib validate", self.print_fib_validate),
            ("breeze fib unicast-routes", self.print_fib_unicast_routes),
            ("breeze fib mpls-routes", self.print_fib_mpls_routes),
            ("breeze fib routes-installed", self.print_fib_routes_installed),
            ("breeze perf fib", self.print_perf_fib),
            ("breeze monitor counters", self.print_monitor_counters),
            ("breeze monitor logs", self.print_monitor_logs),
        ]
        failures = []
        for title, func in funcs:
            self.print_title(title)
            try:
                func()
            except Exception as e:
                failures.append(title)
                print(e, file=sys.stderr)
        if failures:
            self.print_title("openr-tech-support failures")
            print("\n".join(failures))

        ret = 1 if failures else 0
        sys.exit(ret)

    def print_title(self, title):
        print("\n--------  {}  --------\n".format(title))

    def print_config_file(self):
        if not os.path.isfile(Consts.OPENR_CONFIG_FILE):
            print("Missing Config File")
            return
        with open(Consts.OPENR_CONFIG_FILE) as f:
            print(f.read())

    def print_runtime_params(self):
        output = subprocess.check_output(
            ["pgrep", "-a", "openr"], stderr=subprocess.STDOUT
        )
        print(output)

    def print_openr_version(self):
        openr.VersionCmd(self.cli_opts).run(False)

    def print_config(self):
        config.ConfigPrefixAllocatorCmd(self.cli_opts).run()
        config.ConfigLinkMonitorCmd(self.cli_opts).run()
        config.ConfigPrefixManagerCmd(self.cli_opts).run()

    def print_lm_links(self):
        lm.LMLinksCmd(self.cli_opts).run(False, False)

    def print_kvstore_peers(self):
        kvstore.PeersCmd(self.cli_opts).run()

    def print_kvstore_nodes(self):
        kvstore.NodesCmd(self.cli_opts).run()

    def print_kvstore_prefixes(self):
        kvstore.PrefixesCmd(self.cli_opts).run(["all"], False)

    def print_kvstore_keys(self):
        kvstore.KeysCmd(self.cli_opts).run(False, "", originator=None, ttl=True)

    def print_decision_adjs(self):
        decision.DecisionAdjCmd(self.cli_opts).run({"all"}, {"all"}, True, False)

    def print_decision_validate(self):
        decision.DecisionValidateCmd(self.cli_opts).run()

    def print_decision_routes(self):
        if not self.print_routes:
            return
        nodes = parse_nodes(self.cli_opts, "")
        decision.DecisionRoutesComputedCmd(self.cli_opts).run(nodes, [], [], False)

    def print_fib_validate(self):
        fib.FibValidateRoutesCmd(self.cli_opts).run(self.cli_opts)

    def print_fib_unicast_routes(self):
        if not self.print_routes:
            return
        fib.FibUnicastRoutesCmd(self.cli_opts).run([], False)

    def print_fib_mpls_routes(self):
        if not self.print_routes:
            return
        fib.FibMplsRoutesCmd(self.cli_opts).run([], False)

    def print_fib_routes_installed(self):
        if not self.print_routes:
            return
        fib.FibRoutesInstalledCmd(self.cli_opts).run([])

    def print_perf_fib(self):
        perf.ViewFibCmd(self.cli_opts).run()

    def print_monitor_counters(self):
        monitor.CountersCmd(self.cli_opts).run()

    def print_monitor_logs(self):
        monitor.LogCmd(self.cli_opts).run()
