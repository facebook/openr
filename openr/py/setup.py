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
    top_dirs = [
        os.path.join(root_dir, "openr/if"),
        os.path.join(root_dir, "common"),
        os.path.join(root_dir, "fbzmq"),
    ]
    exclude_files = ["OpenrCtrlCpp"]

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
                    "--out",
                    current_dir,
                    str(thrift_file),
                ]
            )


generate_thrift_files()

# Have different install_requires for Python 2 + 3
INSTALL_REQUIRES = ["bunch", "click", "hexdump", "networkx", "pyzmq", "tabulate"]
# Python 2 always needs these dependencies
if version_info.major < 3:
    INSTALL_REQUIRES.extend(["futures", "ipaddress", "typing"])
# Only add typing before 3.5 for Python 3
if version_info.major == 3 and version_info.minor < 5:
    INSTALL_REQUIRES.append("typing")

setup(
    name="py-openr",
    version="1.0",
    author="Open Routing",
    author_email="openr@fb.com",
    description=(
        "OpenR python tools and bindings. Includes python bindings for various "
        "OpenR modules, CLI tool for interacting with OpenR named as `breeze`."
    ),
    packages=create_package_list("openr") + create_package_list("fb303"),
    entry_points={"console_scripts": ["breeze=openr.cli.breeze:main"]},
    license="MIT License",
    install_requires=INSTALL_REQUIRES,
)
