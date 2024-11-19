#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


from collections.abc import Sequence
from typing import Any, List, Optional

import bunch
import click
from openr.py.openr.cli.clis.baseGroup import deduceCommandGroup

from openr.py.openr.cli.commands import decision
from openr.py.openr.cli.utils.utils import parse_nodes


class DecisionCli:
    def __init__(self):
        self.decision.add_command(PathCli().path)
        self.decision.add_command(DecisionAdjCli().adj)
        self.decision.add_command(DecisionRoutesComputedCli().routes, name="routes")
        self.decision.add_command(DecisionRibPolicyCli().show, name="rib-policy")
        self.decision.add_command(ReceivedRoutesCli().show)
        self.decision.add_command(DecisionValidateCli().validate)
        self.decision.add_command(DecisionPartialAdjCli().show, name="partial-adj")

    @click.group(cls=deduceCommandGroup)
    @click.pass_context
    def decision(ctx):  # noqa: B902
        """CLI tool to peek into Decision module."""
        pass


class PathCli:
    @click.command()
    @click.option(
        "--src", default="", help="source node, " "default will be the current host"
    )
    @click.option(
        "--dst",
        default="",
        help="destination node or prefix, " "default will be the current host",
    )
    @click.option("--max-hop", default=256, help="max hop count")
    @click.option("--area", default=None, help="area identifier")
    @click.pass_obj
    def path(cli_opts, src, dst, max_hop, area):  # noqa: B902
        """path from src to dst"""

        decision.PathCmd(cli_opts).run(src, dst, max_hop, area)


class DecisionRoutesComputedCli:
    @click.command()
    @click.option(
        "--nodes",
        default=None,
        help="Get routes for a list of nodes. Default will get "
        "host's routes. Get routes for all nodes if 'all' is given.",
    )
    @click.option(
        "--labels",
        "-l",
        type=click.INT,
        multiple=True,
        help="Get route for specific labels.",
    )
    @click.option(
        "--hostnames/--no-hostnames",
        default=True,
        show_default=True,
        help="Show Hostnames rather than IP addresses",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.argument("prefixes", nargs=-1, type=str, required=False)
    @click.pass_obj
    def routes(cli_opts, nodes, prefixes, labels, hostnames, json):  # noqa: B902
        """Request the routing table from Decision module"""

        nodes_set = parse_nodes(cli_opts, nodes)
        decision.DecisionRoutesComputedCmd(cli_opts).run(
            nodes_set, prefixes, labels, json, hostnames
        )


class DecisionPartialAdjCli:
    @click.command()
    @click.option("--area", "-a", help="Show partial adj for the given area")
    @click.pass_obj
    def show(cli_opts, area):  # noqa: B902
        """dump the partial adjacencies of an area"""

        decision.DecisionShowPartialAdjCmd(cli_opts).run(area)


class DecisionAdjCli:
    @click.command()
    @click.option(
        "--nodes",
        default=None,
        help="Dump adjacencies for a list of nodes. Default will dump "
        "host's adjs. Dump adjs for all nodes if 'all' is given",
    )
    @click.option(
        "--areas",
        "-a",
        multiple=True,
        default=[],
        help="Dump adjacencies for the given list of areas. Default will dump "
        "adjs for all areas. Multiple areas can be provided by repeatedly using "
        "either of the two valid flags: -a or --areas",
    )
    @click.option("--bidir/--no-bidir", default=True, help="Only bidir adjacencies")
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def adj(cli_opts, nodes, areas, bidir, json):  # noqa: B902
        """dump the link-state adjacencies from Decision module"""

        nodes_set = parse_nodes(cli_opts, nodes)
        decision.DecisionAdjCmd(cli_opts).run(nodes_set, set(areas), bidir, json)


class DecisionValidateCli:
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.option(
        "--suppress-error/--print-all-info",
        default=False,
        help="Only print validation results, without extra info",
    )
    @click.argument("areas", nargs=-1)
    @click.pass_obj
    @click.pass_context
    def validate(
        ctx: click.Context,  # noqa: B902
        cli_opts: bunch.Bunch,
        json: bool,
        suppress_error: bool,
        areas: Sequence[str],
    ) -> None:
        """
        Check all prefix & adj dbs in Decision against that in KvStore

        TODO: Fix json to be combined for all areas ...
        If --json is provided, returns database diffs in the following format.
        "neighbor_down" is a list of nodes not in the inspected node's dump that were expected,
        "neighbor_up" is a list of unexpected nodes in inspected node's dump,
        "neighbor_update" is a list of expected nodes whose metadata are unexpected.
            {
                "neighbor_down": [
                    {
                        "new_adj": null,
                        "old_adj": $inconsistent_node
                    }
                ],
                "neighbor_up": [
                    {
                        "new_adj": $inconsistent_node
                        "old_adj": null
                    }
                ],
                "neighbor_update": [
                    {
                        "new_adj": $inconsistent_node
                        "old_adj": $inconsistent_node
                    }
                ]
            }
        """

        ctx.exit(
            decision.DecisionValidateCmd(cli_opts).run(json, suppress_error, areas)
        )


class DecisionRibPolicyCli:
    @click.command()
    @click.pass_obj
    def show(cli_opts):  # noqa: B902
        """
        Show currently configured RibPolicy
        """

        decision.DecisionRibPolicyCmd(cli_opts).run()


class ReceivedRoutesCli:
    @click.command("received-routes")
    @click.argument("prefix", nargs=-1, type=str)
    @click.option("--node", help="Filter on node name", type=str)
    @click.option("--area", help="Filter on area name", type=str)
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
    @click.pass_obj
    def show(
        cli_opts: bunch.Bunch,  # noqa: B902
        prefix: list[str],
        node: str | None,
        area: str | None,
        detail: bool,
        tag2name: bool,
        json: bool,
    ) -> None:
        """
        Show routes this node is advertising. Will show all by default
        """
        decision.ReceivedRoutesCmd(cli_opts).run(
            prefix, node, area, json, detail, tag2name
        )
