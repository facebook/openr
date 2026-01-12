#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from typing import List

import bunch
import click
from openr.py.openr.cli.clis.baseGroup import deduceCommandGroup
from openr.py.openr.cli.commands import kvstore, lm
from openr.py.openr.cli.utils import utils
from openr.py.openr.cli.utils.utils import parse_nodes


class LMCli:
    def __init__(self):
        # [Show Cmd]
        self.lm.add_command(LMLinksCli().links, name="links")
        self.lm.add_command(LMAdjCli().adj, name="adj")
        self.lm.add_command(LMValidateCli().validate, name="validate")

        # [Hard-Drain] set node overload
        self.lm.add_command(
            SetNodeOverloadCli().set_node_overload, name="set-node-overload"
        )
        self.lm.add_command(
            UnsetNodeOverloadCli().unset_node_overload, name="unset-node-overload"
        )
        # [Hard-Drain] set link overload
        self.lm.add_command(
            SetLinkOverloadCli().set_link_overload, name="set-link-overload"
        )
        self.lm.add_command(
            UnsetLinkOverloadCli().unset_link_overload, name="unset-link-overload"
        )
        # [Soft-Drain] set node metric increment
        self.lm.add_command(
            IncreaseNodeMetricCli().increase_node_metric, name="increase-node-metric"
        )
        self.lm.add_command(
            ClearNodeMetricCli().clear_node_metric,
            name="clear-node-metric-increase",
        )
        # [Soft-Drain] set link metric increment
        self.lm.add_command(
            IncreaseLinkMetricCli().increase_link_metric, name="increase-link-metric"
        )
        self.lm.add_command(
            ClearLinkMetricCli().clear_link_metric,
            name="clear-link-metric-increase",
        )
        # [Metric Override]
        self.lm.add_command(
            OverrideAdjMetricCli().override_adj_metric, name="override-adj-metric"
        )
        self.lm.add_command(
            ClearAdjMetricOverrideCli().clear_adj_metric_override,
            name="clear-adj-metric-override",
        )

    @click.group(cls=deduceCommandGroup)
    @click.pass_context
    def lm(ctx):  # noqa: B902
        """CLI tool to peek into Link Monitor module."""
        pass


class LMValidateCli:
    @click.command()
    @click.pass_obj
    def validate(cli_opts):  # noqa: B902
        """Run checks on discovered interfaces"""

        lm.LMValidateCmd(cli_opts).run()


class LMLinksCli:
    @click.command()
    @click.option(
        "--only-suppressed",
        default=False,
        is_flag=True,
        help="Only show suppressed links",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def links(cli_opts, only_suppressed, json):  # noqa: B902
        """Dump all known links of the current host"""

        lm.LMLinksCmd(cli_opts).run(only_suppressed, json)


class LMAdjCli:
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.argument("areas", nargs=-1)
    @click.pass_obj
    def adj(cli_opts: bunch.Bunch, json: bool, areas: list[str]):  # noqa: B902
        """Dump all formed adjacencies of the current host"""

        nodes = parse_nodes(cli_opts, "")
        lm.LMAdjCmd(cli_opts).run(nodes, json, areas)


"""
[Hard-Drain]
    - Node Level Overload;
    - Link Level Overload;
"""


class SetNodeOverloadCli:
    @click.command()
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.pass_obj
    def set_node_overload(cli_opts, yes):  # noqa: B902
        """Set overload bit to stop transit traffic through node."""

        lm.SetNodeOverloadCmd(cli_opts).run(yes)


class UnsetNodeOverloadCli:
    @click.command()
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.pass_obj
    def unset_node_overload(cli_opts, yes):  # noqa: B902
        """Unset overload bit to resume transit traffic through node."""

        lm.UnsetNodeOverloadCmd(cli_opts).run(yes)


class SetLinkOverloadCli:
    @click.command()
    @click.argument("interface")
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.pass_obj
    def set_link_overload(cli_opts, interface, yes):  # noqa: B902
        """Set overload bit for a link. Transit traffic will be drained."""

        lm.SetLinkOverloadCmd(cli_opts).run(interface, yes)


class UnsetLinkOverloadCli:
    @click.command()
    @click.argument("interface")
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.pass_obj
    def unset_link_overload(cli_opts, interface, yes):  # noqa: B902
        """Unset overload bit for a link to allow transit traffic."""

        lm.UnsetLinkOverloadCmd(cli_opts).run(interface, yes)


"""
[Soft-Drain]
    - Node Level Metric Increment
    - Link Level Metric Increment
"""


class IncreaseNodeMetricCli:
    @click.command()
    @click.argument("metric")
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.option("--quiet", is_flag=True, help="Do not print out the links table")
    @click.pass_obj
    def increase_node_metric(cli_opts, metric, yes, quiet):  # noqa: B902
        """
        Increase node-level metric for soft-drain behavior.
        """

        # increase node metric
        lm.IncreaseNodeMetricCmd(cli_opts).run(metric, yes)

        # show adj metric result
        if not quiet:
            nodes = parse_nodes(cli_opts, "")
            lm.LMAdjCmd(cli_opts).run(nodes, False)


class ClearNodeMetricCli:
    @click.command()
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.option("--quiet", is_flag=True, help="Do not print out the links table")
    @click.pass_obj
    def clear_node_metric(cli_opts, yes, quiet):  # noqa: B902
        """
        Clear node-level metric increment for soft-drain behavior.
        """

        # clear node metric increment
        lm.ClearNodeMetricCmd(cli_opts).run(yes)

        # show adj metric result
        if not quiet:
            nodes = parse_nodes(cli_opts, "")
            lm.LMAdjCmd(cli_opts).run(nodes, False)


class IncreaseLinkMetricCli:
    @click.command()
    @click.argument("interface", nargs=-1, required=True)
    @click.argument("metric")
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.option("--quiet", is_flag=True, help="Do not print out the links table")
    @click.pass_obj
    def increase_link_metric(cli_opts, interface, metric, yes, quiet):  # noqa: B902
        """
        Increase link-level metric for soft-drain behavior.
        """

        # increase link metric
        lm.IncreaseLinkMetricCmd(cli_opts).run(interface, metric, yes)

        # show adj metric result
        if not quiet:
            nodes = parse_nodes(cli_opts, "")
            lm.LMAdjCmd(cli_opts).run(nodes, False)


class ClearLinkMetricCli:
    @click.command()
    @click.argument("interface", nargs=-1, required=True)
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.option("--quiet", is_flag=True, help="Do not print out the links table")
    @click.pass_obj
    def clear_link_metric(cli_opts, interface, yes, quiet):  # noqa: B902
        """
        Clear link-level metric increment for soft-drain behavior.
        """

        # clear link metric increment
        lm.ClearLinkMetricCmd(cli_opts).run(interface, yes)

        # show adj metric result
        if not quiet:
            nodes = parse_nodes(cli_opts, "")
            lm.LMAdjCmd(cli_opts).run(nodes, False)


class OverrideAdjMetricCli:
    @click.command()
    @click.argument("node")
    @click.argument("interface")
    @click.argument("metric")
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.option("--quiet", is_flag=True, help="Do not print out the links table")
    @click.pass_obj
    def override_adj_metric(
        cli_opts,
        node,
        interface,
        metric,
        yes,
        quiet,  # noqa: B902
    ):
        """
        Override the adjacency metric value.
        """
        question_str = "Are you sure to override metric for adjacency {} {} ?".format(
            node, interface
        )
        if not utils.yesno(question_str, yes):
            return

        lm.OverrideAdjMetricCmd(cli_opts).run(node, interface, metric, yes)

        if not quiet:
            nodes = parse_nodes(cli_opts, "")
            kvstore.ShowAdjNodeCmd(cli_opts).run(nodes, node, interface)


class ClearAdjMetricOverrideCli:
    @click.command()
    @click.argument("node")
    @click.argument("interface")
    @click.option("--yes", is_flag=True, help="Make command non-interactive")
    @click.option("--quiet", is_flag=True, help="Do not print out the links table")
    @click.pass_obj
    def clear_adj_metric_override(cli_opts, node, interface, yes, quiet):  # noqa: B902
        """
        Clear previously overridden adjacency metric value.
        """
        question_str = "Are you sure to unset metric for adjacency {} {} ?".format(
            node, interface
        )
        if not utils.yesno(question_str, yes):
            return

        lm.ClearAdjMetricOverrideCmd(cli_opts).run(node, interface, yes)
        if not quiet:
            nodes = parse_nodes(cli_opts, "")
            kvstore.ShowAdjNodeCmd(cli_opts).run(nodes, node, interface)
