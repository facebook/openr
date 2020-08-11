#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import os
from pathlib import Path
from subprocess import check_call
from sys import version_info

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
    root_dir = os.path.dirname(os.path.dirname(current_dir))
    top_dirs = [os.path.join(root_dir, "openr/if"), os.path.join(root_dir, "common")]
    exclude_files = ["OpenrCtrlCpp"]

    def get_include_dir(base_path, pattern):
        for dir_name in Path(base_path).rglob("*/usr/include/"):
            # ONLY dir which contains "pattern" is the targeted path
            if sorted(dir_name.rglob(pattern)):
                return str(dir_name.absolute())
        return ""

    # find "usr/include" dir which contains "fbzmq" for thrift generation usage
    fbzmq_thrift_pattern = "fbzmq/service/if/*.thrift"
    usr_include_dir = get_include_dir(os.path.dirname(root_dir), fbzmq_thrift_pattern)

    for top_dir in top_dirs:
        for thrift_file in Path(top_dir).rglob("*.thrift"):
            if thrift_file.stem in exclude_files:
                continue
            print("> Generating python definition for {}".format(thrift_file))
            check_call(
                [
                    "thrift1",
                    "--gen",
                    "py",
                    "-I",
                    root_dir,
                    "-I",
                    usr_include_dir,
                    "--out",
                    current_dir,
                    str(thrift_file),
                ]
            )


generate_thrift_files()

INSTALL_REQUIRES = ["bunch", "click", "hexdump", "networkx", "tabulate"]

setup(
    name="py-openr",
    version="1.0",
    author="Open Routing",
    author_email="openr@fb.com",
    description=(
        "OpenR python tools and bindings. Includes python bindings for various "
        + "OpenR modules, CLI tool for interacting with OpenR named as `breeze`."
    ),
    packages=create_package_list("openr") + create_package_list("fb303"),
    entry_points={"console_scripts": ["breeze=openr.cli.breeze:main"]},
    license="MIT License",
    install_requires=INSTALL_REQUIRES,
    python_requires=">=3.6",
)
