#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


from typing import List, Optional

import bunch
import click
from openr.py.openr.cli.clis.baseGroup import deduceCommandGroup
from openr.py.openr.cli.commands import prefix_mgr
from openr.thrift.OpenrCtrl import thrift_types as ctrl_types


class PrefixMgrCli:
    def __init__(self):
        self.prefixmgr.add_command(AdvertisedRoutesCli().show)
        self.prefixmgr.add_command(OriginatedRoutesCli().show)
        self.prefixmgr.add_command(PrefixMgrValidateCli().validate)

    @click.group(cls=deduceCommandGroup)
    @click.pass_context
    def prefixmgr(ctx):  # noqa: B902
        """CLI tool to peek into Prefix Manager module."""
        pass


class AdvertisedRoutesCli:
    @click.group("advertised-routes", cls=deduceCommandGroup)
    @click.option(
        "--prefix-type",
        "-t",
        help="Filter on source of origination. e.g. RIB, BGP, LINK_MONITOR",
    )
    @click.option(
        "--detail/--no-detail",
        "-d/-D",
        default=True,
        help="Show all details including tags and area-stack",
    )
    @click.option(
        "--tag2name/--no-tag2name",
        "-n/-N",
        default=True,
        help="Translate tag string to human readable name",
    )
    @click.option("--json/--no-json", default=False, help="Output in JSON format")
    @click.pass_context
    def show(
        ctx: bunch.Bunch,  # noqa: B902
        prefix_type: str | None,
        detail: bool,
        tag2name: bool,
        json: bool,
    ) -> None:
        """
        Show advertised routes in various stages of policy
        """

        # Set options & arguments in cli_opts
        if ctx.obj is None:
            ctx.obj = bunch.Bunch()
        ctx.obj["advertised_routes_options"] = bunch.Bunch(
            prefix_type=prefix_type,
            detail=detail,
            json=json,
            tag2name=tag2name,
        )

    @show.command("all")
    @click.argument("prefix", nargs=-1, type=str, required=False)
    @click.pass_obj
    def all(cli_opts: bunch.Bunch, prefix: list[str]) -> None:  # noqa: B902
        """
        Show routes that this node should be advertising across all areas. This
        is pre-area-policy routes. Note this does not show routes denied by origination policy
        """

        opts = cli_opts.advertised_routes_options
        prefix_mgr.AdvertisedRoutesCmd(cli_opts).run(
            prefix, opts.prefix_type, opts.json, opts.detail
        )

    @show.command("pre-policy")
    @click.argument("area", type=str)
    @click.argument("prefix", nargs=-1, type=str, required=False)
    @click.pass_obj
    def pre_area_policy(
        cli_opts: bunch.Bunch, area: str, prefix: list[str]  # noqa: B902
    ) -> None:
        """
        Show pre-policy routes for advertisment of specified area
        but after applying origination, if applicable
        """

        opts = cli_opts.advertised_routes_options
        prefix_mgr.AreaAdvertisedRoutesCmd(cli_opts).run(
            area,
            ctrl_types.RouteFilterType.PREFILTER_ADVERTISED,
            prefix,
            opts.prefix_type,
            opts.json,
            opts.detail,
        )

    @show.command("post-policy")
    @click.argument("area", type=str)
    @click.argument("prefix", nargs=-1, type=str, required=False)
    @click.pass_obj
    def post_area_policy(
        cli_opts: bunch.Bunch, area: str, prefix: list[str]  # noqa: B902
    ) -> None:
        """
        Show post-policy routes that are advertisment to specified area
        """

        opts = cli_opts.advertised_routes_options
        prefix_mgr.AreaAdvertisedRoutesCmd(cli_opts).run(
            area,
            ctrl_types.RouteFilterType.POSTFILTER_ADVERTISED,
            prefix,
            opts.prefix_type,
            opts.json,
            opts.detail,
        )

    @show.command("rejected")
    @click.argument("area", type=str)
    @click.argument("prefix", nargs=-1, type=str, required=False)
    @click.pass_obj
    def rejected_on_area(
        cli_opts: bunch.Bunch, area: str, prefix: list[str]  # noqa: B902
    ) -> None:
        """
        Show routes rejected by area policy on advertisement
        """

        opts = cli_opts.advertised_routes_options
        prefix_mgr.AreaAdvertisedRoutesCmd(cli_opts).run(
            area,
            ctrl_types.RouteFilterType.REJECTED_ON_ADVERTISE,
            prefix,
            opts.prefix_type,
            opts.json,
            opts.detail,
        )


class OriginatedRoutesCli:
    @click.command("originated-routes")
    @click.option(
        "--detail/--no-detail",
        "-d/-D",
        default=True,
        help="Show all details including tags and area-stack",
    )
    @click.option(
        "--tag2name/--no-tag2name",
        "-n/-N",
        default=True,
        help="Translate tag string to human readable name",
    )
    @click.pass_obj
    def show(
        cli_opts: bunch.Bunch,  # noqa: B902
        detail: bool,
        tag2name: bool,
    ) -> None:
        """
        Show originated routes configured on this node. Will show all by default
        """

        prefix_mgr.OriginatedRoutesCmd(cli_opts).run(detail, tag2name)


class PrefixMgrValidateCli:
    @click.command()
    @click.pass_obj
    def validate(cli_opts):
        """Runs validation checks on prefix manager module"""

        prefix_mgr.ValidateCmd(cli_opts).run()
