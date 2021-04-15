#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from typing import List, Optional

import click
from openr.cli.commands import prefix_mgr
from openr.OpenrCtrl import ttypes as ctrl_types


class PrefixMgrCli(object):
    def __init__(self):
        self.prefixmgr.add_command(WithdrawCli().withdraw)
        self.prefixmgr.add_command(AdvertiseCli().advertise)
        self.prefixmgr.add_command(ViewCli().view)
        self.prefixmgr.add_command(SyncCli().sync)
        self.prefixmgr.add_command(AdvertisedRoutesCli().show)
        self.prefixmgr.add_command(OriginatedRoutesCli().show)
        self.prefixmgr.add_command(PrefilterAdvertisedRoutesCli().show)
        self.prefixmgr.add_command(PostfilterAdvertisedRoutesCli().show)
        self.prefixmgr.add_command(RejectedOnAdvertiseRoutesCli().show)

    @click.group()
    @click.pass_context
    def prefixmgr(ctx):  # noqa: B902
        """ CLI tool to peek into Prefix Manager module. """
        pass


class WithdrawCli(object):
    @click.command()
    @click.argument("prefixes", nargs=-1)
    @click.option(
        "--prefix-type",
        "-t",
        default="BREEZE",
        help="Type or client-ID associated with prefix.",
    )
    @click.pass_obj
    def withdraw(cli_opts, prefixes: List[str], prefix_type: str):  # noqa: B902
        """ Withdraw the prefixes being advertised from this node """

        # pyre-fixme[6]: Expected `Bunch` for 1st param but got `WithdrawCli`.
        prefix_mgr.WithdrawCmd(cli_opts).run(prefixes, prefix_type)


class AdvertiseCli(object):
    @click.command()
    @click.argument("prefixes", nargs=-1)
    @click.option(
        "--prefix-type",
        "-t",
        default="BREEZE",
        help="Type or client-ID associated with prefix.",
    )
    @click.option(
        "--forwarding-type",
        default="IP",
        help="Use label forwarding instead of IP forwarding in data path",
    )
    @click.pass_obj
    def advertise(cli_opts, prefixes, prefix_type, forwarding_type):  # noqa: B902
        """ Advertise the prefixes from this node with specific type """

        prefix_mgr.AdvertiseCmd(cli_opts).run(prefixes, prefix_type, forwarding_type)


class SyncCli(object):
    @click.command()
    @click.argument("prefixes", nargs=-1)
    @click.option(
        "--prefix-type",
        "-t",
        default="BREEZE",
        help="Type or client-ID associated with prefix.",
    )
    @click.option(
        "--forwarding-type",
        default="IP",
        help="Use label forwarding instead of IP forwarding in data path",
    )
    @click.pass_obj
    def sync(cli_opts, prefixes, prefix_type, forwarding_type):  # noqa: B902
        """ Sync the prefixes from this node with specific type """

        prefix_mgr.SyncCmd(cli_opts).run(prefixes, prefix_type, forwarding_type)


class ViewCli(object):
    @click.command()
    @click.pass_obj
    def view(cli_opts):  # noqa: B902
        """
        View the prefix of this node
        TODO: Deprecated. Use advertised-routes instead
        """

        prefix_mgr.ViewCmd(cli_opts).run()


class AdvertisedRoutesCli(object):
    @click.command("advertised-routes")
    @click.argument("prefix", nargs=-1, type=str)
    @click.option(
        "--prefix-type",
        "-t",
        help="Filter on source of origination. e.g. RIB, BGP, LINK_MONITOR",
    )
    @click.option(
        "--detail/--no-detail",
        default=False,
        help="Show all details including tags and area-stack",
    )
    @click.option("--json/--no-json", default=False, help="Output in JSON format")
    @click.pass_obj
    def show(
        cli_opts,  # noqa: B902
        prefix: List[str],
        prefix_type: Optional[str],
        detail: bool,
        json: bool,
    ) -> None:
        """
        Show routes this node is advertising. Will show all by default
        """

        # pyre-fixme[6]: Expected `Bunch` for 1st param but got `AdvertisedRoutesCli`.
        prefix_mgr.AdvertisedRoutesCmd(cli_opts).run(prefix, prefix_type, json, detail)


class OriginatedRoutesCli(object):
    @click.command("originated-routes")
    @click.pass_obj
    def show(
        cli_opts,  # noqa: B902
    ) -> None:
        """
        Show originated routes configured on this node. Will show all by default
        """

        # pyre-fixme[6]: Expected `Bunch` for 1st param but got `OriginatedRoutesCli`.
        prefix_mgr.OriginatedRoutesCmd(cli_opts).run()


class PrefilterAdvertisedRoutesCli(object):
    @click.command("prefilter-advertised")
    @click.argument("area", type=str)
    @click.argument("prefix", nargs=-1, type=str)
    @click.option(
        "--prefix-type",
        "-t",
        help="Filter on source of origination. e.g. RIB, BGP, LINK_MONITOR",
    )
    @click.option(
        "--detail/--no-detail",
        default=False,
        help="Show all details including tags and area-stack",
    )
    @click.option("--json/--no-json", default=False, help="Output in JSON format")
    @click.pass_obj
    def show(
        cli_opts,  # noqa: B902
        area: str,
        prefix: List[str],
        prefix_type: Optional[str],
        detail: bool,
        json: bool,
    ) -> None:
        """
        Given an area, show prefilter advertised routes to the area. Will show all by default
        """

        # pyre-fixme[6]: Expected `Bunch` for 1st param but got
        #  `PrefilterAdvertisedRoutesCli`.
        prefix_mgr.AreaAdvertisedRoutesCmd(cli_opts).run(
            area,
            ctrl_types.RouteFilterType.PREFILTER_ADVERTISED,
            prefix,
            prefix_type,
            json,
            detail,
        )


class PostfilterAdvertisedRoutesCli(object):
    @click.command("postfilter-advertised")
    @click.argument("area", type=str)
    @click.argument("prefix", nargs=-1, type=str)
    @click.option(
        "--prefix-type",
        "-t",
        help="Filter on source of origination. e.g. RIB, BGP, LINK_MONITOR",
    )
    @click.option(
        "--detail/--no-detail",
        default=False,
        help="Show all details including tags, area-stack and accept policy",
    )
    @click.option("--json/--no-json", default=False, help="Output in JSON format")
    @click.pass_obj
    def show(
        cli_opts,  # noqa: B902
        area: str,
        prefix: List[str],
        prefix_type: Optional[str],
        detail: bool,
        json: bool,
    ) -> None:
        """
        Given an area, show postfilter advertised routes to the area. Will show all by default
        """

        # pyre-fixme[6]: Expected `Bunch` for 1st param but got
        #  `PostfilterAdvertisedRoutesCli`.
        prefix_mgr.AreaAdvertisedRoutesCmd(cli_opts).run(
            area,
            ctrl_types.RouteFilterType.POSTFILTER_ADVERTISED,
            prefix,
            prefix_type,
            json,
            detail,
        )


class RejectedOnAdvertiseRoutesCli(object):
    @click.command("postfilter-rejected")
    @click.argument("area", type=str)
    @click.argument("prefix", nargs=-1, type=str)
    @click.option(
        "--prefix-type",
        "-t",
        help="Filter on source of origination. e.g. RIB, BGP, LINK_MONITOR",
    )
    @click.option(
        "--detail/--no-detail",
        default=False,
        help="Show all details including tags, area-stack and reject policy",
    )
    @click.option("--json/--no-json", default=False, help="Output in JSON format")
    @click.pass_obj
    def show(
        cli_opts,  # noqa: B902
        area: str,
        prefix: List[str],
        prefix_type: Optional[str],
        detail: bool,
        json: bool,
    ) -> None:
        """
        Given an area, show routes rejected on advertise to the area. Will show all by default
        """

        # pyre-fixme[6]: Expected `Bunch` for 1st param but got
        #  `RejectedOnAdvertiseRoutesCli`.
        prefix_mgr.AreaAdvertisedRoutesCmd(cli_opts).run(
            area,
            ctrl_types.RouteFilterType.REJECTED_ON_ADVERTISE,
            prefix,
            prefix_type,
            json,
            detail,
        )
