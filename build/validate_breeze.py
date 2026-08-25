# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
from pathlib import Path


FORBIDDEN_OPENR_ARTIFACT_SUFFIXES = (".cpp", ".pyx", ".so")


def validate_package_contents(package_root):
    openr_package_root = Path(package_root, "openr")
    forbidden_artifacts = [
        path
        for path in openr_package_root.rglob("*")
        if path.is_file() and path.suffix in FORBIDDEN_OPENR_ARTIFACT_SUFFIXES
    ]
    if forbidden_artifacts:
        raise RuntimeError(
            "Breeze unexpectedly contains native Open/R artifacts: "
            + ", ".join(str(path) for path in forbidden_artifacts)
        )


def validate_breeze(package_root):
    validate_package_contents(package_root)

    # Load generated modules only after the installed package layout is valid.
    from fb303_core.thrift_clients import BaseService
    from neteng.config.routing_policy.thrift_types import BgpCommunity
    from openr.py.openr.cli import breeze
    from openr.thrift.Dual.thrift_types import DualMessages
    from openr.thrift.KvStore.thrift_types import Publication
    from openr.thrift.Network.thrift_types import IpPrefix
    from openr.thrift.OpenrConfig.thrift_types import KvstoreFloodRate
    from openr.thrift.OpenrCtrl.thrift_reflection import get_reflection__OpenrError
    from openr.thrift.OpenrCtrl.thrift_types import OpenrError
    from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp
    from openr.thrift.Platform.thrift_clients import FibService
    from openr.thrift.Types.thrift_types import AdjacencyDatabase
    from thrift.python.serializer import deserialize, serialize

    flood_rate = KvstoreFloodRate(
        flood_msg_per_sec=100,
        flood_msg_burst_size=200,
    )
    encoded = serialize(flood_rate)
    if deserialize(KvstoreFloodRate, encoded) != flood_rate:
        raise RuntimeError("Breeze Thrift round-trip validation failed")
    if get_reflection__OpenrError() is None:
        raise RuntimeError("Breeze Thrift reflection validation failed")

    for generated_type in (
        BaseService,
        FibService,
        OpenrCtrlCpp,
        AdjacencyDatabase,
        BgpCommunity,
        DualMessages,
        IpPrefix,
        OpenrError,
        Publication,
    ):
        if generated_type is None:
            raise RuntimeError("Breeze generated module validation failed")

    if breeze.get_breeze_cli().name != "breeze":
        raise RuntimeError("Breeze CLI validation failed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-root", required=True)
    args = parser.parse_args()
    validate_breeze(args.package_root)


if __name__ == "__main__":
    main()
