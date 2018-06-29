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

import os
import subprocess
import sys

from openr.utils.consts import Consts
from openr.cli.commands import config, decision, fib, kvstore, lm, monitor
from openr.cli.commands import perf, prefix_mgr
from openr.cli.utils.utils import parse_nodes


class TechSupportCmd(object):

    def __init__(self, cli_opts):
        ''' initialize the tech support command '''
        self.cli_opts = cli_opts
        # Keep short timeout
        self.cli_opts.timeout = 1000
        # Print routes or not
        self.print_routes = False

    def run(self, routes):
        self.print_routes = routes
        funcs = [
            ('openr config file', self.print_config_file),
            ('openr runtime params', self.print_runtime_params),
            ('openr version', self.print_openr_version),
            ('openr build information', self.print_build_info),
            ('openr config', self.print_config),
            ('breeze prefixmgr view', self.print_prefixmgr_view),
            ('breeze lm links', self.print_lm_links),
            ('breeze kvstore peers', self.print_kvstore_peers),
            ('breeze kvstore nodes', self.print_kvstore_nodes),
            ('breeze kvstore adj', self.print_kvstore_adjs),
            ('breeze kvstore prefixes', self.print_kvstore_prefixes),
            ('breeze kvstore keys --ttl', self.print_kvstore_keys),
            ('breeze decision validate', self.print_decision_validate),
            ('breeze decision routes', self.print_decision_routes),
            ('breeze fib validate', self.print_fib_validate),
            ('breeze fib routes-computed', self.print_fib_routes_computed),
            ('breeze fib routes-installed', self.print_fib_routes_installed),
            ('breeze perf fib', self.print_perf_fib),
            ('breeze monitor counters', self.print_monitor_counters),
            ('breeze monitor logs', self.print_monitor_logs),
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
            self.print_title('openr-tech-support failures')
            print('\n'.join(failures))

        print()
        return -1 if failures else 0

    def print_title(self, title):
        print('\n--------  {}  --------\n'.format(title))

    def print_config_file(self):
        if not os.path.isfile(Consts.OPENR_CONFIG_FILE):
            print('Missing Config File')
            return
        with open(Consts.OPENR_CONFIG_FILE) as f:
            print(f.read())

    def print_runtime_params(self):
        output = subprocess.check_output(
            ['pgrep', '-a', 'openr'], stderr=subprocess.STDOUT)
        print(output)

    def print_openr_version(self):
        lm.VersionCmd(self.cli_opts).run(False)

    def print_build_info(self):
        lm.BuildInfoCmd(self.cli_opts).run(False)

    def print_config(self):
        config.ConfigPrefixAllocatorCmd(self.cli_opts).run()
        config.ConfigLinkMonitorCmd(self.cli_opts).run()
        config.ConfigPrefixManagerCmd(self.cli_opts).run()

    def print_prefixmgr_view(self):
        prefix_mgr.ViewCmd(self.cli_opts).run()

    def print_lm_links(self):
        lm.LMLinksCmd(self.cli_opts).run(True, False)

    def print_kvstore_peers(self):
        kvstore.PeersCmd(self.cli_opts).run()

    def print_kvstore_nodes(self):
        kvstore.NodesCmd(self.cli_opts).run()

    def print_kvstore_adjs(self):
        kvstore.AdjCmd(self.cli_opts).run(['all'], False, False)

    def print_kvstore_prefixes(self):
        kvstore.PrefixesCmd(self.cli_opts).run(['all'], False)

    def print_kvstore_keys(self):
        kvstore.KeysCmd(self.cli_opts).run(False, '', originator=None, ttl=True)

    def print_decision_validate(self):
        decision.DecisionValidateCmd(self.cli_opts).run()

    def print_decision_routes(self):
        if not self.print_routes:
            return
        nodes = parse_nodes(self.cli_opts.host, '', self.cli_opts.lm_cmd_port)
        decision.DecisionRoutesComputedCmd(self.cli_opts).run(nodes, [], False)

    def print_fib_validate(self):
        fib.FibValidateRoutesCmd(self.cli_opts).run(self.cli_opts)

    def print_fib_routes_computed(self):
        if not self.print_routes:
            return
        fib.FibRoutesComputedCmd(self.cli_opts).run([], False)

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
