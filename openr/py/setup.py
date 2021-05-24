#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import glob
import os

from Cython.Build import cythonize
from setuptools import Extension, find_packages, setup


INSTALL_REQUIRES = [
    "bunch",
    "click",
    "cython",
    "hexdump",
    "jsondiff",
    "networkx",
    "six",
    "tabulate",
]


def create_package_list(base):
    """
    Get all packages under the base directory
    """

    return [base] + ["{}.{}".format(base, pkg) for pkg in find_packages(base)]


extension_include_dirs = [
    ".",
    "/opt/facebook/folly/include",
    *glob.glob("/opt/facebook/boost-*/include"),
    *glob.glob("/opt/facebook/glog-*/include"),
    *glob.glob("/opt/facebook/gflags-*/include"),
    *glob.glob("/opt/facebook/double-*/include"),
    *glob.glob("/opt/facebook/libevent-*/include"),
    "/opt/facebook/fbthrift/include",
    "/opt/facebook/fizz/include",
    "/opt/facebook/wangle/include/",
    *glob.glob("/opt/facebook/fmt-*/include"),
    *glob.glob("/opt/facebook/libsodium-*/include"),
    "/tmp/fbcode_builder_getdeps-ZsrcZbuildZfbcode_builder-root/build/openr",
]

bespoke_extensions = [
    "openr.thrift.OpenrCtrlCpp.clients",
    "openr.thrift.OpenrCtrl.clients",
    "openr.thrift.Platform.clients",
]

extension_library_dirs = [
    "/usr/lib/",
    "/opt/facebook/openr/lib/",
    "/opt/facebook/fbthrift/lib/",
    "/opt/facebook/folly/lib/",
]

extension_libraries = ["openrlib", "thriftcpp2", "folly"]


extensions = [
    Extension(
        "openr.thrift.OpenrCtrlCpp.clients",
        [
            "./openr-thrift/openr/if/gen-py3/openr/thrift/OpenrCtrlCpp/clients.cpp",
            "openr-thrift/openr/if/gen-py3/OpenrCtrlCpp/clients_wrapper.cpp",
        ],
        library_dirs=extension_library_dirs,
        include_dirs=extension_include_dirs,
        libraries=extension_libraries,
    ),
    Extension(
        "openr.thrift.OpenrCtrl.clients",
        [
            "./openr-thrift/openr/if/gen-py3/openr/thrift/OpenrCtrl/clients.cpp",
            "openr-thrift/openr/if/gen-py3/OpenrCtrl/clients_wrapper.cpp",
        ],
        library_dirs=extension_library_dirs,
        include_dirs=extension_include_dirs,
        libraries=extension_libraries,
    ),
    Extension(
        "openr.thrift.Platform.clients",
        [
            "./openr-thrift/openr/if/gen-py3/openr/thrift/Platform/clients.cpp",
            "openr-thrift/openr/if/gen-py3/Platform/clients_wrapper.cpp",
        ],
        library_dirs=extension_library_dirs,
        include_dirs=extension_include_dirs,
        libraries=extension_libraries,
    ),
]

for root, _dirs, files in os.walk("openr-thrift"):
    for f in files:
        if f.endswith(".pyx"):
            pyx_file = os.path.join(root, f)
            module = (
                pyx_file.replace("openr-thrift/openr/if/gen-py3/", "")
                .replace(".pyx", "")
                .replace("/", ".")
            )
            cpp_file = pyx_file.replace("pyx", "cpp")
            if module in bespoke_extensions:
                continue
            extensions += [
                Extension(
                    module,
                    [cpp_file],
                    library_dirs=extension_library_dirs,
                    include_dirs=extension_include_dirs,
                    libraries=extension_libraries,
                ),
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
    packages=create_package_list("openr"),
    entry_points={"console_scripts": ["breeze=openr.cli.breeze:main"]},
    license="MIT License",
    install_requires=INSTALL_REQUIRES,
    ext_modules=cythonize(extensions, compiler_directives={"language_level": "3"}),
    python_requires=">=3.7",
)
