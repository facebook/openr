# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
from subprocess import check_call


def generate_thrift_files():
    """
    Get list of all thrift files (absolute path names) and then generate
    python definitions for all thrift files.
    """

    thrift_dirs = [
        "openr-thrift",
        "fb303-thrift",
        "fbzmq-thrift",
        "fbthrift-thrift",
        "neteng-thrift",
    ]
    generators = ["mstch_cpp2", "py", "mstch_py3"]
    includes = [
        "openr-thrift",
        "fb303-thrift",
        "fbzmq-thrift",
        "neteng-thrift",
        "fbthrift-thrift",
        ".",
    ]

    # Find .thrift files
    thrift_files = []
    for thrift_dir in thrift_dirs:
        for root, _dirs, files in os.walk(thrift_dir):
            for file in files:
                if file.endswith(".thrift"):
                    thrift_files += [os.path.join(root, file)]

    # Generate cpp and python
    for gen in generators:
        cmd = ["/opt/facebook/fbthrift/bin/thrift1", "--gen", gen]
        for include in includes:
            cmd += ["-I", f"{include}"]
        for thrift_file in thrift_files:
            check_call(
                [
                    *cmd,
                    "-o",
                    os.path.join(os.path.dirname(thrift_file)),
                    str(thrift_file),
                ]
            )

    # Add __init__.py for compiling cython modules
    for include in includes:
        for root, _dirs, files in os.walk(f"{include}"):
            for f in files:
                check_call(
                    ["touch", os.path.join(root, os.path.dirname(f), "__init__.py")]
                )


generate_thrift_files()
