#!/usr/bin/env python3
#
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import argparse
import os
import select
import signal
import tempfile
import textwrap
import threading
from subprocess import PIPE, Popen

import libfb.py.pathutils as pathutils
from nsenter import Namespace


class FbMeshd(threading.Thread):
    def __init__(self, namespace, encrypted=True, verbosity=0, logfile=None):

        self.fbmeshd = pathutils.get_build_rule_output_path(
            "//openr/fbmeshd:fbmeshd", pathutils.BuildRuleTypes.CXX_BINARY
        )
        self.encrypted = encrypted
        cfg = self.gen_config(encrypted=self.encrypted, verbosity=verbosity)
        self.cmd = [
            self.fbmeshd,
            "-config",
            cfg,
            "-v",
            str(verbosity),
            "-logtostderr",
            "1",
            "-enable_openr_metric_manager=true",
        ]
        self.namespace = namespace
        self.logfile = logfile
        self.verbosity = verbosity
        threading.Thread.__init__(self)

    def run(self):
        with Namespace(self.namespace, "net"):
            self.proc = Popen(self.cmd, stderr=PIPE)
            p = select.poll()
            p.register(self.proc.stderr, select.POLLIN)
            more = True
            while more:
                if p.poll():  # blocking call
                    line = self.proc.stderr.readline()
                    if self.verbosity > 0:
                        print(line.decode("utf-8"), end="")
                    more = len(line) > 0
                else:
                    more = False
            self.proc.stderr.close()

    def stop(self):
        self.proc.terminate()
        self.proc.wait()

    def gen_config(self, encrypted=False, verbosity=0):
        encstr = "true" if encrypted else "false"
        debugmask = 965 if verbosity else 0
        fd, path = tempfile.mkstemp(text=True, prefix="fbmeshd")
        with os.fdopen(fd, mode="w") as f:
            f.write(
                textwrap.dedent(
                    """
                    {{
                      "meshList": [
                        {{
                          "meshId": "bazookaTEST",
                          "ifName": "mesh0",
                          "frequency": 2412,
                          "channelType": "HT20",
                          "rssiThreshold": -80,
                          "maxPeerLinks": 13,
                          "encryption": {{
                            "enabled": {},
                            "password": "thisisreallyquitesecretisitnot",
                            "saeGroups": [19, 26, 21, 25, 20],
                            "debugVerbosity": {}
                          }}
                        }}
                      ]
                    }}
                    """.format(
                        encstr, debugmask
                    )
                )
            )
        return path


def sighandler(signum, frame):
    fbmd.stop()


if __name__ == "__main__":

    parser = argparse.ArgumentParser()
    parser.add_argument("--verbosity", type=int, default=0)
    args = parser.parse_args()

    signal.signal(signal.SIGTERM, sighandler)
    signal.signal(signal.SIGINT, sighandler)
    # launch on default namespace (pid=1)
    fbmd = FbMeshd(1, verbosity=args.verbosity)
    fbmd.run()
