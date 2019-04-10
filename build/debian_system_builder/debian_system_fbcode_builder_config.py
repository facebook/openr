#!/usr/bin/env python

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import, division, print_function, unicode_literals

import debian_specs.fbzmq as fbzmq
import specs.fbthrift as fbthrift
import specs.folly as folly
import specs.re2 as re2
from shell_quoting import ShellQuoted, path_join


"fbcode_builder steps to build & test Openr"


def fbcode_builder_spec(builder):
    builder.add_option("thom311/libnl:git_hash", "libnl3_2_25")
    builder.add_option("openr/build:cmake_defines", {"ADD_ROOT_TESTS": "OFF"})
    maybe_curl_patch = []
    patch = path_join(
        builder.option("projects_dir"),
        "../shipit_projects/openr/build/fix-route-obj-attr-list.patch",
    )

    if not builder.has_option("shipit_project_dir"):
        maybe_curl_patch = [
            builder.run(
                ShellQuoted(
                    "curl -O https://raw.githubusercontent.com/facebook/openr/master/"
                    "build/fix-route-obj-attr-list.patch"
                )
            )
        ]
        patch = "fix-route-obj-attr-list.patch"
    libnl_build_commands = maybe_curl_patch + [
        builder.run(ShellQuoted("git apply {p}").format(p=patch)),
        builder.run(ShellQuoted("./autogen.sh")),
        builder.configure(),
        builder.make_and_install(),
    ]

    return {
        "depends_on": [folly, fbthrift, fbzmq, re2],
        "steps": [
            builder.github_project_workdir("thom311/libnl", "."),
            builder.step("Build and install thom311/libnl", libnl_build_commands),
            builder.fb_github_project_workdir("openr/build", "facebook"),
            builder.step(
                "Build and install openr/build",
                [
                    builder.cmake_configure("openr/build"),
                    # we need the pythonpath to find the thrift compiler
                    builder.run(
                        ShellQuoted(
                            'PYTHONPATH="$PYTHONPATH:"{p}/lib/python2.7/site-packages '
                            "make -j {n}"
                        ).format(
                            p=builder.option("prefix"),
                            n=builder.option("make_parallelism"),
                        )
                    ),
                    builder.run(ShellQuoted("sudo make install")),
                    builder.run(ShellQuoted("sudo ldconfig")),
                ],
            ),
            builder.step(
                "Run openr tests",
                [builder.run(ShellQuoted("CTEST_OUTPUT_ON_FAILURE=TRUE make test"))],
            ),
        ],
    }


config = {
    "github_project": "facebook/openr",
    "fbcode_builder_spec": fbcode_builder_spec,
}
