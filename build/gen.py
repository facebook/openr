# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import shutil
from subprocess import check_call


# Breeze uses OpenrConfig V1; V2 needs separate routing-policy-v2 packaging.
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

REQUIRED_DEPENDENCY_THRIFT_FILES = (
    "fb303-thrift/fb303/thrift/fb303_core.thrift",
    "fbthrift-thrift/thrift/annotation/cpp.thrift",
    "fbthrift-thrift/thrift/annotation/hack.thrift",
    "fbthrift-thrift/thrift/annotation/python.thrift",
    "fbthrift-thrift/thrift/annotation/scope.thrift",
    "fbthrift-thrift/thrift/annotation/thrift.thrift",
    "neteng-thrift/configerator/structs/neteng/config/routing_policy.thrift",
)

THRIFT_INCLUDE_DIRS = (
    "openr-thrift",
    "fb303-thrift",
    "neteng-thrift",
    "fbthrift-thrift",
    ".",
)

GENERATED_THRIFT_DIR = "generated-thrift"
THRIFT_COMPILER = "/opt/facebook/fbthrift/bin/thrift1"


def collect_thrift_files(source_root="."):
    """Return the supported Open/R roots and required generated dependencies."""

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

    thrift_files.extend(
        os.path.join(source_root, thrift_file)
        for thrift_file in REQUIRED_DEPENDENCY_THRIFT_FILES
    )
    missing_files = [path for path in thrift_files if not os.path.isfile(path)]
    if missing_files:
        raise FileNotFoundError(
            "Missing required Breeze Thrift files: " + ", ".join(missing_files)
        )

    for include_dir in THRIFT_INCLUDE_DIRS:
        include_root = os.path.join(source_root, include_dir)
        if not os.path.isdir(include_root):
            raise FileNotFoundError(
                f"Missing Breeze Thrift include directory: {include_root}"
            )

    return thrift_files


def generate_thrift_files(source_root=".", thrift_compiler=THRIFT_COMPILER):
    """Generate modern Thrift Python modules into one namespace-package tree."""

    output_root = os.path.join(source_root, GENERATED_THRIFT_DIR)
    if os.path.exists(output_root):
        shutil.rmtree(output_root)
    os.makedirs(output_root, exist_ok=True)
    cmd = [thrift_compiler, "--gen", "mstch_python"]
    for include in THRIFT_INCLUDE_DIRS:
        cmd.extend(["-I", os.path.join(source_root, include)])
    for thrift_file in collect_thrift_files(source_root):
        check_call([*cmd, "-o", output_root, thrift_file])

    return os.path.join(output_root, "gen-python")


if __name__ == "__main__":
    generate_thrift_files()
