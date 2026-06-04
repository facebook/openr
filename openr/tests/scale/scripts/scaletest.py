# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

"""CLI client for the OpenR scale-test Thrift server (`scale_test_server_d`).

Subcommands:
  start --config <yaml>   Start a session from a YAML config file.
  stop                    Stop the active session.
  status                  Print the test status.
  nodes                   List node names in the active session.
  down <node>             Down a node.
  up <node>               Up a node.
  down-link <a> <b>       Down the adjacency between two nodes.
  up-link <a> <b>         Up the adjacency between two nodes.
  counters [regex]        Print DUT counters, optionally filtered by regex.
  neighbor-stats          Print simulated Spark-neighbor stats.
  verify-routes           Print the DUT's computed route counts.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
from typing import Any, Mapping

import yaml
from openr.tests.scale.ScaleTestServer.thrift_clients import ScaleTestServer
from openr.tests.scale.ScaleTestServer.thrift_types import (
    DutConnection,
    DutRole,
    InjectionConfig,
    ScaleTestConfig,
    TopologyConfig,
)
from thrift.python.client import get_client


def _build_config(doc: Mapping[str, Any]) -> ScaleTestConfig:
    """Convert a parsed YAML mapping into a ScaleTestConfig.

    Only fields present in the YAML are set; everything else defaults from the
    Thrift IDL.
    """
    dut_in = doc.get("dut", {}) or {}
    topo_in = doc.get("topology", {}) or {}
    inj_in = doc.get("injection", {}) or {}

    def _require(section: dict, key: str, section_name: str) -> Any:
        if key not in section:
            raise ValueError(
                f"YAML config missing required field: {section_name}.{key}"
            )
        return section[key]

    dut = DutConnection(
        host=_require(dut_in, "host", "dut"),
        port=int(dut_in.get("port", 2018)),
    )

    dut_role_str = str(_require(topo_in, "dutRole", "topology")).upper()
    if dut_role_str not in ("SPINE", "LEAF"):
        raise ValueError(
            f"topology.dutRole must be 'SPINE' or 'LEAF' (got '{dut_role_str}')"
        )
    topology = TopologyConfig(
        type=topo_in.get("type", "bbf-simple"),
        dutRole=DutRole[dut_role_str],
        numSpines=int(_require(topo_in, "numSpines", "topology")),
        numLeaves=int(_require(topo_in, "numLeaves", "topology")),
        numSuperSpines=int(_require(topo_in, "numSuperSpines", "topology")),
        numPods=int(_require(topo_in, "numPods", "topology")),
        numSites=int(_require(topo_in, "numSites", "topology")),
        numPrefixesPerNode=int(_require(topo_in, "numPrefixesPerNode", "topology")),
        ecmpWidth=int(_require(topo_in, "ecmpWidth", "topology")),
    )

    injection = InjectionConfig(
        injectTopology=bool(_require(inj_in, "injectTopology", "injection")),
        simulateNeighbors=bool(_require(inj_in, "simulateNeighbors", "injection")),
        enableFakeKvStore=bool(_require(inj_in, "enableFakeKvStore", "injection")),
        fakeKvStoreBasePort=(
            int(inj_in["fakeKvStoreBasePort"])
            if "fakeKvStoreBasePort" in inj_in
            else None
        ),
        numFakeKeysPerNode=(
            int(inj_in["numFakeKeysPerNode"])
            if "numFakeKeysPerNode" in inj_in
            else None
        ),
        fakeKeyVersionBumpIntervalSec=(
            int(inj_in["fakeKeyVersionBumpIntervalSec"])
            if "fakeKeyVersionBumpIntervalSec" in inj_in
            else None
        ),
        interfaces=(list(inj_in["interfaces"]) if "interfaces" in inj_in else None),
    )

    return ScaleTestConfig(dut=dut, topology=topology, injection=injection)


def _format_status(status: Any) -> str:
    lines = [
        f"running: {status.running}",
        f"dutConnected: {status.dutConnected}",
    ]
    if status.elapsedSec is not None:
        lines.append(f"elapsedSec: {status.elapsedSec}")
    if status.neighborCount is not None:
        lines.append(f"neighborCount: {status.neighborCount}")
    if status.downedNodes:
        lines.append("downedNodes:")
        for n in status.downedNodes:
            lines.append(f"  - {n}")
    if status.downedLinks:
        lines.append("downedLinks:")
        for lr in status.downedLinks:
            lines.append(f"  - {lr.localNode} <-> {lr.remoteNode}")
    if status.activeConfig is not None:
        cfg = status.activeConfig
        lines.append("activeConfig:")
        lines.append(f"  dut: {cfg.dut.host}:{cfg.dut.port}")
        lines.append(
            f"  topology: type={cfg.topology.type} dutRole={cfg.topology.dutRole.name} "
            f"spines={cfg.topology.numSpines} leaves={cfg.topology.numLeaves} "
            f"pods={cfg.topology.numPods}"
        )
    return "\n".join(lines)


async def _cmd_start(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    with open(args.config) as f:
        doc = yaml.safe_load(f) or {}
    cfg = _build_config(doc)
    await client.startTest(cfg)
    print("started")
    return 0


async def _cmd_stop(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    await client.stopTest()
    print("stopped")
    return 0


async def _cmd_status(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    status = await client.getTestStatus()
    print(_format_status(status))
    return 0


async def _cmd_nodes(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    names = await client.listNodes()
    for n in names:
        print(n)
    return 0


async def _cmd_down(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    await client.downNode(args.node)
    print(f"down: {args.node}")
    return 0


async def _cmd_up(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    await client.upNode(args.node)
    print(f"up: {args.node}")
    return 0


async def _cmd_down_link(
    client: ScaleTestServer.Async, args: argparse.Namespace
) -> int:
    await client.downLink(args.a, args.b)
    print(f"down-link: {args.a} <-> {args.b}")
    return 0


async def _cmd_up_link(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    await client.upLink(args.a, args.b)
    print(f"up-link: {args.a} <-> {args.b}")
    return 0


async def _cmd_counters(client: ScaleTestServer.Async, args: argparse.Namespace) -> int:
    # Server filters by regex (empty == default counter set).
    counters = await client.getDutCounters(args.regex or "")
    for key in sorted(counters):
        print(f"{key}\t{counters[key]}")
    return 0


def _format_neighbor_stats(stats: Any) -> str:
    lines = [
        f"hellos:      sent={stats.hellosSent} recv={stats.hellosReceived}",
        f"handshakes:  sent={stats.handshakesSent} recv={stats.handshakesReceived}",
        f"heartbeats:  sent={stats.heartbeatsSent} recv={stats.heartbeatsReceived}",
        f"parseErrors: {stats.parseErrors}",
        f"established:  {stats.neighborsEstablished} / {stats.totalNeighbors}",
    ]
    if stats.neighbors:
        lines.append("")
        lines.append(f"{'NEIGHBOR':<20} {'STATE':<15} {'DUT':<20} FAILED")
        for n in stats.neighbors:
            dut = n.dutNode or "(unknown)"
            lines.append(
                f"{n.name:<20} {n.state:<15} {dut:<20} {'YES' if n.failed else 'no'}"
            )
    return "\n".join(lines)


async def _cmd_neighbor_stats(
    client: ScaleTestServer.Async, args: argparse.Namespace
) -> int:
    print(_format_neighbor_stats(await client.getNeighborStats()))
    return 0


async def _cmd_verify_routes(
    client: ScaleTestServer.Async, args: argparse.Namespace
) -> int:
    counts = await client.verifyRoutes()
    print(f"unicastRoutes: {counts.unicastRoutes}")
    print(f"mplsRoutes: {counts.mplsRoutes}")
    return 0


def _get_handlers() -> dict[str, Any]:
    return {
        "start": _cmd_start,
        "stop": _cmd_stop,
        "status": _cmd_status,
        "nodes": _cmd_nodes,
        "down": _cmd_down,
        "up": _cmd_up,
        "down-link": _cmd_down_link,
        "up-link": _cmd_up_link,
        "counters": _cmd_counters,
        "neighbor-stats": _cmd_neighbor_stats,
        "verify-routes": _cmd_verify_routes,
    }


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="scaletest",
        description="CLI client for the OpenR scale-test Thrift server.",
    )
    parser.add_argument(
        "--host", default="::1", help="Thrift server host (default: ::1)"
    )
    parser.add_argument(
        "--port", type=int, default=2019, help="Thrift server port (default: 2019)"
    )

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_start = sub.add_parser("start", help="Start a session from a YAML config")
    p_start.add_argument("--config", required=True, help="Path to YAML config file")

    sub.add_parser("stop", help="Stop the active session")
    sub.add_parser("status", help="Print test status")
    sub.add_parser("nodes", help="List node names")

    p_down = sub.add_parser("down", help="Down a node")
    p_down.add_argument("node")
    p_up = sub.add_parser("up", help="Up a node")
    p_up.add_argument("node")

    p_dl = sub.add_parser("down-link", help="Down an adjacency")
    p_dl.add_argument("a")
    p_dl.add_argument("b")
    p_ul = sub.add_parser("up-link", help="Up an adjacency")
    p_ul.add_argument("a")
    p_ul.add_argument("b")

    p_c = sub.add_parser("counters", help="Print DUT counters")
    p_c.add_argument(
        "regex",
        nargs="?",
        default=None,
        help="Optional regex to filter counter keys",
    )

    sub.add_parser("neighbor-stats", help="Print simulated Spark neighbor stats")
    sub.add_parser("verify-routes", help="Print DUT computed route counts")

    return parser.parse_args(argv)


async def _main_async(args: argparse.Namespace) -> int:
    handler = _get_handlers()[args.cmd]
    async with get_client(ScaleTestServer, host=args.host, port=args.port) as client:
        return await handler(client, args)


def main() -> int:
    args = _parse_args(sys.argv[1:])
    try:
        return asyncio.run(_main_async(args))
    except Exception as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
