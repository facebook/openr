# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
from subprocess import check_call


# Breeze uses OpenrConfig V1; V2 needs separate native and dependency packaging.
SUPPORTED_OPENR_THRIFT_FILES = (
    "Dual.thrift",
    "KvStore.thrift",
    "Network.thrift",
    "OpenrConfig.thrift",
    "OpenrCtrl.thrift",
    "OpenrCtrlCpp.thrift",
    "Platform.thrift",
    "Types.thrift",
)

EXCLUDED_OPENR_THRIFT_FILES = ("OpenrConfigV2.thrift",)

DEPENDENCY_THRIFT_DIRS = (
    "fb303-thrift",
    "neteng-thrift",
    "fbthrift-thrift",
)

REQUIRED_DEPENDENCY_THRIFT_FILES = (
    "neteng-thrift/configerator/structs/neteng/config/routing_policy.thrift",
    "neteng-thrift/configerator/structs/neteng/config/vip_service_config.thrift",
)

THRIFT_INCLUDE_DIRS = (
    "openr-thrift",
    *DEPENDENCY_THRIFT_DIRS,
    ".",
)


def collect_thrift_files(source_root="."):
    """Return the supported Open/R roots and their staged dependencies."""

    openr_thrift_dir = os.path.join(source_root, "openr-thrift", "openr", "if")
    actual_openr_thrift_files = {
        file for file in os.listdir(openr_thrift_dir) if file.endswith(".thrift")
    }
    expected_openr_thrift_files = set(SUPPORTED_OPENR_THRIFT_FILES).union(
        EXCLUDED_OPENR_THRIFT_FILES
    )
    if actual_openr_thrift_files != expected_openr_thrift_files:
        raise RuntimeError(
            "Update the Breeze Thrift manifest for: "
            + ", ".join(
                sorted(
                    actual_openr_thrift_files.symmetric_difference(
                        expected_openr_thrift_files
                    )
                )
            )
        )

    thrift_files = [
        os.path.join(openr_thrift_dir, thrift_file)
        for thrift_file in SUPPORTED_OPENR_THRIFT_FILES
    ]

    required_dependency_files = [
        os.path.join(source_root, thrift_file)
        for thrift_file in REQUIRED_DEPENDENCY_THRIFT_FILES
    ]
    missing_files = [
        path
        for path in [*thrift_files, *required_dependency_files]
        if not os.path.isfile(path)
    ]
    if missing_files:
        raise FileNotFoundError(
            "Missing required Breeze Thrift files: " + ", ".join(missing_files)
        )

    for thrift_dir in DEPENDENCY_THRIFT_DIRS:
        dependency_root = os.path.join(source_root, thrift_dir)
        if not os.path.isdir(dependency_root):
            raise FileNotFoundError(
                f"Missing Breeze Thrift dependency directory: {dependency_root}"
            )
        for root, dirs, files in os.walk(dependency_root):
            dirs.sort()
            for file in sorted(files):
                if file.endswith(".thrift"):
                    thrift_files.append(os.path.join(root, file))

    return thrift_files


def generate_thrift_files():
    """
    Get list of all thrift files (absolute path names) and then generate
    python definitions for all thrift files.
    """

    generators = ["mstch_cpp2", "py", "mstch_py3"]
    thrift_files = collect_thrift_files()

    # Generate cpp and python
    for gen in generators:
        cmd = ["/opt/facebook/fbthrift/bin/thrift1", "--gen", gen]
        for include in THRIFT_INCLUDE_DIRS:
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
    for include in THRIFT_INCLUDE_DIRS:
        for root, _dirs, files in os.walk(f"{include}"):
            for f in files:
                check_call(
                    ["touch", os.path.join(root, os.path.dirname(f), "__init__.py")]
                )


if __name__ == "__main__":
    generate_thrift_files()
