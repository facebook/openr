#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from setuptools import find_packages, setup


INSTALL_REQUIRES = [
    "bunch",
    "click",
    "cython",
    "hexdump",
    "jsondiff",
    "networkx",
    "six",
    "tabulate"
]


def create_package_list(base):
    """
    Get all packages under the base directory
    """

    return [base] + ["{}.{}".format(base, pkg) for pkg in find_packages(base)]


setup(
    name="openr",
    version="2.0.0",
    author="Open Routing",
    author_email="openr@fb.com",
    description=(
        "OpenR python tools and bindings. Includes python bindings for various "
        + "OpenR modules, CLI tool for interacting with OpenR named as `breeze`."
    ),
    packages=create_package_list("openr"),
    entry_points={"console_scripts": ["breeze=openr.cli.breeze:main"]},
    license="MIT License",
    install_requires=INSTALL_REQUIRES,
    python_requires=">=3.7",
)
