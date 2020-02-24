#!/usr/bin/env python
# Copyright (c) Facebook, Inc. and its affiliates.
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import debian_specs.fbzmq as fbzmq

from shell_quoting import ShellQuoted, path_join


def fbcode_builder_spec(builder):
    return {
        'depends_on': [fbzmq],
        'steps': [
            builder.step(
                "Install fbzmq python modules",
                [
                    builder.workdir(
                        path_join(
                            builder.option('projects_dir'),
                            "fbzmq/fbzmq/py"
                        )
                    ),
                    builder.run(
                        ShellQuoted(
                            "sudo python setup.py install"
                        )
                    ),
                ]
            ),
        ],
    }
