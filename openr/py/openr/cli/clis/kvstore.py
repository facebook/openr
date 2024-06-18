#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


from typing import AbstractSet, Any, List, Optional, Set

import click
from bunch import Bunch
from openr.cli.clis.baseGroup import deduceCommandGroup
from openr.cli.commands import kvstore
from openr.py.openr.cli.utils.options import breeze_option
from openr.py.openr.cli.utils.utils import parse_nodes
from openr.py.openr.utils.consts import Consts


class KvStoreCli:
    def __init__(self):
        self.kvstore.add_command(PrefixesCli().prefixes)
        self.kvstore.add_command(NodesCli().nodes)
        self.kvstore.add_command(KeysCli().keys)
        self.kvstore.add_command(KeyValsCli().keyvals)
        self.kvstore.add_command(KvCompareCli().kv_compare, name="kv-compare")
        self.kvstore.add_command(PeersCli().peers)
        self.kvstore.add_command(EraseKeyCli().erase_key, name="erase-key")
        self.kvstore.add_command(SetKeyCli().set_key, name="set-key")
        self.kvstore.add_command(KvSignatureCli().kv_signature, name="kv-signature")
        self.kvstore.add_command(SnoopCli().snoop)
        self.kvstore.add_command(AreasCli().areas, name="areas")
        self.kvstore.add_command(SummaryCli().summary)
        self.kvstore.add_command(
            StreamSummaryCli().stream_summary, name="stream-summary"
        )
        self.kvstore.add_command(ValidateCli().validate)

    @click.group(cls=deduceCommandGroup)
    @breeze_option("--area", type=str, help="area identifier")
    @click.pass_context
    def kvstore(ctx, area):  # noqa: B902
        """CLI tool to peek into KvStore module."""
        pass


class ValidateCli:
    @click.command()
    @click.pass_obj
    def validate(cli_opts):
        """Run validation checks"""

        kvstore.ValidateCmd(cli_opts).run()


class PrefixesCli:
    @click.command()
    @click.option(
        "--nodes",
        default="",
        help="Dump prefixes for a list of nodes. Default will dump host's "
        "prefixes. Dump prefixes for all nodes if 'all' is given.",
    )
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.option("--prefix", "-p", default="", help="Prefix filter. Exact match")
    @click.option(
        "--client-type",
        "-c",
        default="",
        help="Client type filter. Provide name e.g. loopback, bgp",
    )
    @click.pass_obj
    def prefixes(
        cli_opts: Any,  # noqa: B902
        nodes: str,
        json: bool,
        prefix: str,
        client_type: str,
    ) -> None:
        """show the prefixes in the network"""

        nodes_set = parse_nodes(cli_opts, nodes)
        kvstore.PrefixesCmd(cli_opts).run(nodes_set, json, prefix, client_type)


class KeysCli:
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.option("--prefix", default="", help="string to filter keys")
    @click.option("--originator", default=None, help="originator string to filter keys")
    @click.option(
        "--ttl/--no-ttl", default=False, help="Show ttl value and version as well"
    )
    @click.pass_obj
    def keys(cli_opts, json, prefix, originator, ttl):  # noqa: B902
        """dump all available keys"""

        kvstore.KeysCmd(cli_opts).run(json, prefix, originator, ttl)


class KeyValsCli:
    @click.command()
    @click.argument("keys", nargs=-1, required=True)
    @click.pass_obj
    def keyvals(cli_opts, keys):  # noqa: B902
        """get values of input keys"""

        kvstore.KeyValsCmd(cli_opts).run(keys)


class NodesCli:
    @click.command()
    @click.pass_obj
    def nodes(cli_opts):  # noqa: B902
        """show nodes info"""

        kvstore.NodesCmd(cli_opts).run()


class AreasCli:
    @click.command()
    @click.option("--json/--no-json", default=False, help="Dump in JSON format")
    @click.pass_obj
    def areas(cli_opts: Bunch, json) -> None:  # noqa: B902
        """get list of 'areas' configured"""
        kvstore.Areas(cli_opts).run(json)


class KvCompareCli:
    @click.command()
    @click.option(
        "--nodes",
        default="",
        help="Kv-compare the current host with a list of nodes. "
        "Compare with all the other nodes if 'all' is given. "
        "Default will kv-compare against each peer.",
    )
    @click.pass_obj
    def kv_compare(cli_opts, nodes):  # noqa: B902
        """get the kv store delta"""

        kvstore.KvCompareCmd(cli_opts).run(nodes)


class PeersCli:
    @click.command()
    @click.pass_obj
    def peers(cli_opts):  # noqa: B902
        """show the KV store peers of the node"""

        kvstore.PeersCmd(cli_opts).run()


class EraseKeyCli:
    @click.command()
    @click.argument("key")
    @click.pass_obj
    def erase_key(cli_opts, key):  # noqa: B902
        """erase key from kvstore"""

        kvstore.EraseKeyCmd(cli_opts).run(key)


class SetKeyCli:
    @click.command()
    @click.argument("key")
    @click.argument("value")
    @click.option("--originator", default="breeze", help="Originator ID")
    @click.option(
        "--version",
        default=None,
        help="Version. If not set, override existing key if any",
    )
    @click.option(
        "--ttl",
        default=Consts.CONST_TTL_INF,
        help="TTL in seconds. Default is infinite",
    )
    @click.pass_obj
    def set_key(cli_opts, key, value, originator, version, ttl):  # noqa: B902
        """Set a custom key into KvStore"""

        if ttl != Consts.CONST_TTL_INF:
            ttl = ttl * 1000
        kvstore.SetKeyCmd(cli_opts).run(key, value, originator, version, ttl)


class KvSignatureCli:
    @click.command()
    @click.option(
        "--prefix",
        default="",
        help="Limit the keys included "
        "in the signature computation to those that begin with "
        "the given prefix",
    )
    @click.pass_obj
    def kv_signature(cli_opts, prefix):  # noqa: B902
        """Returns a signature of the contents of the KV store for comparison
        with other nodes.  In case of mismatch, use kv-compare to analyze
        differences
        """

        kvstore.KvSignatureCmd(cli_opts).run(prefix)


class SnoopCli:
    @click.command()
    @click.option("--delta/--no-delta", default=True, help="Output incremental changes")
    @click.option("--ttl/--no-ttl", default=False, help="Print ttl updates")
    @click.option(
        "--regexes", "-r", default=[], multiple=True, help="Keys to be used in filter"
    )
    @click.option(
        "--duration", default=0, help="How long to snoop for ? Default is infinite"
    )
    @click.option(
        "--match-all/--match-any",
        default=True,
        help="Boolean operator for combining keys and originator ids (default=match-all)",
    )
    @click.option(
        "--originator-ids",
        "-o",
        default=[],
        multiple=True,
        help="Originator ids to be used in filter",
    )
    @click.option(
        "--area",
        "-a",
        multiple=True,
        help="Area to snoop on, if none specified will snoop on all. Specify "
        "multiple times to snoop on a set of areas",
    )
    @click.option(
        "--print-initial", is_flag=True, help="Print initial snapshot before snooping"
    )
    @click.pass_obj
    def snoop(
        cli_opts: Bunch,  # noqa: B902
        delta: bool,
        ttl: bool,
        regexes: Optional[List[str]],
        duration: int,
        originator_ids: Optional[AbstractSet[str]],
        match_all: bool,
        area: Set[str],
        print_initial: bool,
    ) -> None:
        """Snoop on KV-store updates in the network. We are primarily
        looking at the adj/prefix announcements.
        """

        kvstore.SnoopCmd(cli_opts).run(
            delta,
            ttl,
            regexes,
            duration,
            originator_ids,
            match_all,
            area,
            print_initial,
        )


class SummaryCli:
    default_area_list: List[str] = []

    @click.command()
    @click.option(
        "--area",
        "-a",
        multiple=True,
        default=default_area_list,
        help="Dump summaries for the given list of areas. Default will dump "
        "summaries for all areas. Multiple areas can be provided by repeatedly using "
        "either of the two valid flags: -a or --areas",
    )
    @click.pass_obj
    def summary(cli_opts: Bunch, area: List[str]) -> None:  # noqa: B902
        """show the KV store summary for each area"""

        kvstore.SummaryCmd(cli_opts).run(set(area))


class StreamSummaryCli:
    @click.command()
    @click.pass_obj
    def stream_summary(cli_opts):  # noqa: B902
        """Show basic info on all KVstore subscribers"""
        cli_options = {}
        kvstore.StreamSummaryCmd(cli_opts).run(cli_options)
