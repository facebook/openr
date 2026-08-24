# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import shutil
from pathlib import Path


GENERATED_PACKAGE_ROOTS = ("facebook", "fb303_core", "neteng", "openr", "vipconfig")
NON_SERVICE_THRIFT_MODULES = (
    "facebook/thrift/annotation/cpp",
    "facebook/thrift/annotation/hack",
    "facebook/thrift/annotation/python",
    "facebook/thrift/annotation/scope",
    "facebook/thrift/annotation/thrift",
    "neteng/config/routing_policy",
    "openr/thrift/Dual",
    "openr/thrift/Network",
    "openr/thrift/OpenrConfig",
    "openr/thrift/Types",
    "vipconfig/config/vip_service_config",
)
SERVICE_THRIFT_MODULES = (
    "fb303_core",
    "openr/thrift/KvStore",
    "openr/thrift/OpenrCtrl",
    "openr/thrift/OpenrCtrlCpp",
    "openr/thrift/Platform",
)
COMMON_GENERATED_FILES = (
    "thrift_abstract_types.py",
    "thrift_enums.py",
    "thrift_metadata.py",
    "thrift_mutable_types.py",
    "thrift_mutable_types.pyi",
    "thrift_reflection.py",
    "thrift_types.py",
    "thrift_types.pyi",
    "thrift_uris.txt",
)
SERVICE_GENERATED_FILES = (
    "thrift_clients.py",
    "thrift_mutable_clients.py",
    "thrift_mutable_services.py",
    "thrift_services.py",
    "thrift_services_reflection.py",
)


def _expected_generated_files():
    generated_files = [
        Path(module) / filename
        for module in (*NON_SERVICE_THRIFT_MODULES, *SERVICE_THRIFT_MODULES)
        for filename in COMMON_GENERATED_FILES
    ]
    generated_files.extend(
        Path(module) / filename
        for module in SERVICE_THRIFT_MODULES
        for filename in SERVICE_GENERATED_FILES
    )
    return tuple(sorted(generated_files))


EXPECTED_GENERATED_FILES = _expected_generated_files()


def stage_breeze_package(source_root, generated_root, output_root):
    """Assemble the Open/R sources and generated namespaces for setuptools."""

    source_root = Path(source_root)
    generated_root = Path(generated_root)
    output_root = Path(output_root)

    expected_generated_files = set(EXPECTED_GENERATED_FILES)
    actual_generated_files = {
        path.relative_to(generated_root)
        for package_root in GENERATED_PACKAGE_ROOTS
        for path in (generated_root / package_root).rglob("*")
        if path.is_file()
    }
    if actual_generated_files != expected_generated_files:
        missing_files = expected_generated_files - actual_generated_files
        unexpected_files = actual_generated_files - expected_generated_files
        details = []
        if missing_files:
            details.append(
                "missing " + ", ".join(str(path) for path in sorted(missing_files))
            )
        if unexpected_files:
            details.append(
                "unexpected "
                + ", ".join(str(path) for path in sorted(unexpected_files))
            )
        raise RuntimeError("Invalid generated Breeze package: " + "; ".join(details))

    if output_root.exists():
        shutil.rmtree(output_root)

    shutil.copytree(
        source_root / "openr" / "py" / "openr",
        output_root / "openr" / "py" / "openr",
        dirs_exist_ok=True,
    )
    for package_root in GENERATED_PACKAGE_ROOTS:
        shutil.copytree(
            generated_root / package_root,
            output_root / package_root,
            dirs_exist_ok=True,
        )
    shutil.copy2(source_root / "openr" / "py" / "setup.py", output_root)

    return output_root


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--generated-root", required=True)
    parser.add_argument("--output-root", required=True)
    args = parser.parse_args()
    stage_breeze_package(args.source_root, args.generated_root, args.output_root)


if __name__ == "__main__":
    main()
