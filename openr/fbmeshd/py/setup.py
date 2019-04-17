#!/usr/bin/env python3
#
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

import os
from subprocess import check_call

from setuptools import find_packages, setup


def create_package_list(base):
    """
    Get all packages under the base directory
    """

    return [base] + ["{}.{}".format(base, pkg) for pkg in find_packages(base)]


def generate_thrift_files():
    """
    Get list of all thrift files (absolute path names) and then generate
    python definitions for all thrift files.
    """

    current_dir = os.path.dirname(os.path.realpath(__file__))
    thrift_dir = os.path.join(os.path.dirname(current_dir), "if")
    thrift_files = [x for x in os.listdir(thrift_dir) if x.endswith(".thrift")]

    for thrift_file in thrift_files:
        print("> Generating python definition for {}".format(thrift_file))
        check_call(
            [
                "thrift1",
                "--gen",
                "py",
                "--out",
                current_dir,
                os.path.join(thrift_dir, thrift_file),
            ]
        )


generate_thrift_files()

setup(name="fbmeshd", version="1.0", packages=create_package_list("fbmeshd"))
