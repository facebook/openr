#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from setuptools import find_namespace_packages, setup


INSTALL_REQUIRES = [
    "bunch3",
    "click",
    "hexdump",
    "jsondiff",
    "networkx",
    "prettytable",
    "pytz",
    "six",
    "tabulate",
]


setup(
    name="openr",
    version="2.0.0",
    author="Open Routing",
    author_email="openr@fb.com",
    description=(
        "OpenR python tools and bindings. Includes python bindings for various "
        + "OpenR modules, CLI tool for interacting with OpenR named as `breeze`."
    ),
    packages=find_namespace_packages(where="."),
    package_data={"": ["*.pyi", "thrift_uris.txt"]},
    include_package_data=True,
    entry_points={"console_scripts": ["breeze=openr.py.openr.cli.breeze:main"]},
    license="MIT License",
    install_requires=INSTALL_REQUIRES,
    python_requires=">=3.10",
)
