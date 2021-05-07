#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import printing


class VersionCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        json: bool,
        *args,
        **kwargs,
    ) -> None:
        openr_version = client.getOpenrVersion()
        build_info = client.getBuildInfo()

        if json:
            if build_info.buildPackageName:
                info = utils.thrift_to_dict(build_info)
                print(utils.json_dumps(info))
            version = utils.thrift_to_dict(openr_version)
            print(utils.json_dumps(version))
        else:
            if build_info.buildPackageName:
                print("Build Information")
                print("  Built by: {}".format(build_info.buildUser))
                print("  Built on: {}".format(build_info.buildTime))
                print("  Built at: {}".format(build_info.buildHost))
                print("  Build path: {}".format(build_info.buildPath))
                print("  Package Name: {}".format(build_info.buildPackageName))
                print("  Package Version: {}".format(build_info.buildPackageVersion))
                print("  Package Release: {}".format(build_info.buildPackageRelease))
                print("  Build Revision: {}".format(build_info.buildRevision))
                print(
                    "  Build Upstream Revision: {}".format(
                        build_info.buildUpstreamRevision
                    )
                )
                print("  Build Platform: {}".format(build_info.buildPlatform))
                print(
                    "  Build Rule: {} ({}, {}, {})".format(
                        build_info.buildRule,
                        build_info.buildType,
                        build_info.buildTool,
                        build_info.buildMode,
                    )
                )
            rows = []
            rows.append(["Open Source Version", ":", openr_version.version])
            rows.append(
                [
                    "Lowest Supported Open Source Version",
                    ":",
                    openr_version.lowestSupportedVersion,
                ]
            )
            print(
                printing.render_horizontal_table(
                    rows, column_labels=[], tablefmt="plain"
                )
            )
