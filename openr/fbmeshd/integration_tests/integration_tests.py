#!/usr/bin/env python3
#
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import os
import time
import unittest
from subprocess import check_call, check_output

from nsenter import Namespace

from .fbmeshd_launcher import FbMeshd


class TestFbmeshdIntegration(unittest.TestCase):
    def setUp(self):
        lsmod_out = check_output("/usr/sbin/lsmod")
        if b"mac80211_hwsim" in lsmod_out:
            check_call(["/usr/sbin/modprobe", "-r", "mac80211-hwsim"])
        check_call(["/usr/sbin/modprobe", "mac80211-hwsim"])
        self.ns_names = {"phy0": "fbmeshd-ns0", "phy1": "fbmeshd-ns1"}
        self.ns_paths = {
            key: "/var/run/netns/" + name for (key, name) in self.ns_names.items()
        }
        for phy, ns_name in self.ns_names.items():
            if not os.path.exists(self.ns_paths[phy]):
                check_call(["/usr/sbin/ip", "netns", "add", ns_name])
            check_call(["/usr/sbin/iw", "phy", phy, "set", "netns", "name", ns_name])

    def tearDown(self):
        self.stop_fbmeshds()
        check_call(["/usr/sbin/modprobe", "-r", "mac80211-hwsim"])
        for phy in self.ns_paths:
            if os.path.exists(self.ns_paths[phy]):
                check_call(["/usr/sbin/ip", "netns", "delete", self.ns_names[phy]])

    def start_fbmeshds(self, encrypted=False):
        self.fbmd = [FbMeshd(ns, encrypted=encrypted) for ns in self.ns_paths.values()]
        [t.start() for t in self.fbmd]
        time.sleep(1)

    def stop_fbmeshds(self):
        [t.stop() for t in self.fbmd]
        [t.join() for t in self.fbmd]

    def test_open_plink_establishment(self):

        # launch fbmeshd in separate namespaces
        self.start_fbmeshds()

        # now test things in each namespace
        with Namespace(self.ns_paths["phy0"], "net"):
            iw_out = check_output(["/usr/sbin/iw", "mesh0", "station", "dump"])
            self.assertTrue(b"ESTAB" in iw_out)
        with Namespace(self.ns_paths["phy1"], "net"):
            iw_out = check_output(["/usr/sbin/iw", "mesh0", "station", "dump"])
            self.assertTrue(b"ESTAB" in iw_out)

    def test_secure_plink_establishment(self):

        # launch fbmeshd in separate namespaces
        self.start_fbmeshds(encrypted=True)
        time.sleep(10)

        # now test things in each namespace
        with Namespace(self.ns_paths["phy0"], "net"):
            iw_out = check_output(["/usr/sbin/iw", "mesh0", "station", "dump"])
            self.assertTrue(b"ESTAB" in iw_out)
        with Namespace(self.ns_paths["phy1"], "net"):
            iw_out = check_output(["/usr/sbin/iw", "mesh0", "station", "dump"])
            self.assertTrue(b"ESTAB" in iw_out)


if __name__ == "__main__":
    unittest.main()
