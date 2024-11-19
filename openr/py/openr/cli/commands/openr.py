#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from openr.py.openr.cli.commands import decision, fib, kvstore, lm, prefix_mgr, spark
from openr.py.openr.cli.utils import utils
from openr.py.openr.cli.utils.commands import OpenrCtrlCmd
from openr.py.openr.utils import printing, serializer
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient


class VersionCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        json: bool,
        *args,
        **kwargs,
    ) -> None:
        openr_version = await client.getOpenrVersion()
        build_info = await client.getBuildInfo()

        if json:
            if build_info.buildPackageName:
                info = utils.thrift_to_dict(build_info)
                print(utils.json_dumps(info))
            version = utils.thrift_to_dict(openr_version)
            print(utils.json_dumps(version))
        else:
            if build_info.buildPackageName:
                print("Build Information")
                print(f"  Built by: {build_info.buildUser}")
                print(f"  Built on: {build_info.buildTime}")
                print(f"  Built at: {build_info.buildHost}")
                print(f"  Build path: {build_info.buildPath}")
                print(f"  Package Name: {build_info.buildPackageName}")
                print(f"  Package Version: {build_info.buildPackageVersion}")
                print(f"  Package Release: {build_info.buildPackageRelease}")
                print(f"  Build Revision: {build_info.buildRevision}")
                print(
                    "  Build Upstream Revision: {}".format(
                        build_info.buildUpstreamRevision
                    )
                )
                print(f"  Build Platform: {build_info.buildPlatform}")
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


class OpenrValidateCmd(OpenrCtrlCmd):
    # @override
    def run(
        self,
        suppress_error=False,
        json=False,
        *args,
        **kwargs,
    ) -> int:

        spark_pass = spark.ValidateCmd(self.cli_opts).run(detail=False)
        lm_pass = lm.LMValidateCmd(self.cli_opts).run()
        kvstore_pass = kvstore.ValidateCmd(self.cli_opts).run()
        fib_pass = fib.FibValidateRoutesCmd(self.cli_opts).run(
            suppress_error=suppress_error
        )
        decision_pass = decision.DecisionValidateCmd(self.cli_opts).run(
            suppress=suppress_error
        )
        prefixmgr_pass = prefix_mgr.ValidateCmd(self.cli_opts).run()

        if json:
            check_res = {
                "Spark": spark_pass,
                "Link Monitor": lm_pass,
                "KvStore": kvstore_pass == 0,
                "Fib": fib_pass == 0,
                "Decision": decision_pass == 0,
                "Prefix Manager": prefixmgr_pass,
            }
            print(serializer.serialize_json(check_res))

        return 0
