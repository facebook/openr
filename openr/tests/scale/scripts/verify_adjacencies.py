# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

"""Assert the DUT's Spark adjacencies match what the scale tester should produce.

Runs against a DUT's OpenrCtrl endpoint (default port 2018) while
`scale_test_server`/`scale_test_server_d` drives simulated neighbors from the
helper. Intended as the pass/fail gate for scale runs over dot1q sub-interfaces
on a port-channel (see setup_portchannel_subinterfaces.sh).

Checks performed:
  count         Number of ESTABLISHED Spark neighbors equals the expected count.
  distribution  Neighbors are packed onto the supplied interfaces the way
                SparkNeighborDistribution does it: interfaces 0..M-2 get
                floor(N/M) each and the LAST interface absorbs the remainder.
  ctrl-ports    Every neighbor has a distinct openrCtrlThriftPort. Co-located
                neighbors share one link-local, so a duplicate port would
                collapse them into a single KvStore peer.
  linkmonitor   The DUT's own LinkMonitor adjacency count agrees with the Spark
                neighbor count (an independent view of the same fact).

The expected neighbor count mirrors DutPatcher::buildBaseDutNeighborNames:

    N = (dut_role == spine) ? num_leaves + num_super_spines
                            : num_spines + (num_sites > 0 ? 1 : 0)
    N *= max(1, len(areas))

It is deliberately independent of the sub-interface count: SparkFaker packs many
simulated neighbors onto each interface and distinguishes them by node name, not
by source address. Pass --expected to assert a hand-computed number instead.

Examples:
  # 64/252 leaf-role run over 8 sub-interfaces
  verify_adjacencies --dut-host <dut> --num-spines 64 --num-leaves 252 \\
      --num-sites 20 --dut-role leaf --interfaces 8 --wait 120

  # assert an explicit count, skip the topology math
  verify_adjacencies --dut-host <dut> --expected 65 --interfaces 8
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from collections import Counter
from typing import Any, Sequence

from openr.thrift.OpenrCtrl.thrift_types import AdjacenciesFilter
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp
from thrift.python.client import ClientType, get_client

_TIMEOUT_MS = 30 * 1000
_ESTABLISHED = "ESTABLISHED"


class CheckFailure(Exception):
    """A single assertion about the DUT's adjacency state did not hold."""


def expected_neighbor_count(
    dut_role: str,
    num_spines: int,
    num_leaves: int,
    num_super_spines: int,
    num_sites: int,
    num_areas: int,
) -> int:
    """Neighbor count the tester will simulate, per DutPatcher.cpp:24-49."""
    if dut_role == "spine":
        base = num_leaves + num_super_spines
    else:
        # The DUT replaces leaf-0, so its neighbors are the spines plus, when
        # sites exist, the single eb-site-0 node.
        base = num_spines + (1 if num_sites > 0 else 0)
    return base * max(1, num_areas)


def expected_distribution(num_neighbors: int, num_interfaces: int) -> list[int]:
    """Per-interface neighbor counts, per SparkNeighborDistribution.cpp:23-47.

    Contiguous blocks, not round-robin: the first M-1 interfaces get
    floor(N/M) each and the last one absorbs the remainder. With M > N the
    leading interfaces get zero and every neighbor lands on the last one.
    """
    if num_interfaces <= 0:
        raise ValueError(f"num_interfaces must be positive (got {num_interfaces})")
    per_interface = num_neighbors // num_interfaces
    head = [per_interface] * (num_interfaces - 1)
    return head + [num_neighbors - sum(head)]


def _client(host: str, port: int) -> Any:
    return get_client(
        OpenrCtrlCpp,
        host=host,
        port=port,
        timeout=_TIMEOUT_MS,
        client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
    )


def check_count(established: Sequence[Any], expected: int) -> str:
    if len(established) != expected:
        raise CheckFailure(
            f"{len(established)} ESTABLISHED Spark neighbors, expected {expected}"
        )
    return f"{expected} ESTABLISHED Spark neighbors"


def check_distribution(established: Sequence[Any], num_interfaces: int) -> str:
    want = sorted(expected_distribution(len(established), num_interfaces))
    by_interface = Counter(n.localIfName for n in established)
    got = sorted(by_interface.values())
    # Interfaces carrying zero neighbors never appear in getNeighbors(), so pad
    # the observed counts back out to M before comparing.
    got = [0] * (num_interfaces - len(got)) + got
    if got != want:
        raise CheckFailure(
            f"neighbor distribution {got} != expected {want} "
            f"(per-interface: {dict(by_interface)})"
        )
    return f"distribution across {num_interfaces} interfaces: {want}"


def check_ctrl_ports(established: Sequence[Any]) -> str:
    ports = [n.openrCtrlThriftPort for n in established]
    duplicates = [p for p, count in Counter(ports).items() if count > 1]
    if duplicates:
        raise CheckFailure(
            f"duplicate openrCtrlThriftPort values {sorted(duplicates)} — "
            "co-located neighbors would collapse into one KvStore peer"
        )
    return f"{len(set(ports))} distinct neighbor ctrl ports"


def _count_linkmonitor_adjacencies(area_dbs: Any) -> int:
    return sum(
        len(db.adjacencies)
        for dbs in area_dbs.values()
        for db in dbs  # noqa: B905
    )


def check_linkmonitor(adjacency_count: int, established: Sequence[Any]) -> str:
    if adjacency_count != len(established):
        raise CheckFailure(
            f"LinkMonitor reports {adjacency_count} adjacencies but Spark "
            f"reports {len(established)} ESTABLISHED neighbors"
        )
    return f"LinkMonitor agrees: {adjacency_count} adjacencies"


async def _sample(host: str, port: int) -> tuple[list[Any], int]:
    """Return (ESTABLISHED spark neighbors, LinkMonitor adjacency count)."""
    async with _client(host, port) as client:
        neighbors = await client.getNeighbors()
        area_dbs = await client.getLinkMonitorAreaAdjacenciesFiltered(
            AdjacenciesFilter(selectAreas=set())
        )
    established = [n for n in neighbors if n.state == _ESTABLISHED]
    return established, _count_linkmonitor_adjacencies(area_dbs)


async def _wait_for_count(
    host: str, port: int, expected: int, timeout_sec: int, interval_sec: int
) -> tuple[list[Any], int]:
    """Poll until the ESTABLISHED count reaches `expected` or time runs out.

    Returns the last sample either way; the caller's checks decide pass/fail, so
    a timeout still produces an actionable count rather than a bare error.
    """
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout_sec
    established, adjacencies = await _sample(host, port)
    while len(established) != expected and loop.time() < deadline:
        print(
            f"  waiting for convergence: {len(established)}/{expected} ESTABLISHED",
            file=sys.stderr,
        )
        await asyncio.sleep(interval_sec)
        established, adjacencies = await _sample(host, port)
    return established, adjacencies


def _resolve_expected(args: argparse.Namespace) -> int:
    if args.expected is not None:
        return args.expected
    return expected_neighbor_count(
        dut_role=args.dut_role,
        num_spines=args.num_spines,
        num_leaves=args.num_leaves,
        num_super_spines=args.num_super_spines,
        num_sites=args.num_sites,
        num_areas=args.num_areas,
    )


def _run_checks(
    established: Sequence[Any],
    adjacency_count: int,
    expected: int,
    num_interfaces: int | None,
) -> list[tuple[str, bool, str]]:
    checks: list[tuple[str, Any]] = [
        ("count", lambda: check_count(established, expected)),
        ("ctrl-ports", lambda: check_ctrl_ports(established)),
        ("linkmonitor", lambda: check_linkmonitor(adjacency_count, established)),
    ]
    if num_interfaces is not None:
        checks.insert(
            1, ("distribution", lambda: check_distribution(established, num_interfaces))
        )

    results: list[tuple[str, bool, str]] = []
    for name, fn in checks:
        try:
            results.append((name, True, fn()))
        except CheckFailure as ex:
            results.append((name, False, str(ex)))
    return results


def _report(results: Sequence[tuple[str, bool, str]], as_json: bool) -> None:
    if as_json:
        print(
            json.dumps(
                {
                    "passed": all(ok for _, ok, _ in results),
                    "checks": [
                        {"name": name, "passed": ok, "detail": detail}
                        for name, ok, detail in results
                    ],
                },
                indent=2,
            )
        )
        return
    for name, ok, detail in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="verify_adjacencies",
        description="Assert DUT Spark adjacencies match the scale tester's topology.",
    )
    parser.add_argument("--dut-host", required=True, help="DUT hostname or IP")
    parser.add_argument(
        "--dut-port", type=int, default=2018, help="DUT OpenrCtrl port (default: 2018)"
    )
    parser.add_argument(
        "--expected",
        type=int,
        default=None,
        help="Expected ESTABLISHED count; overrides the topology math below",
    )
    parser.add_argument(
        "--dut-role",
        choices=("spine", "leaf"),
        default="leaf",
        help="DUT role in the simulated topology (default: leaf)",
    )
    parser.add_argument("--num-spines", type=int, default=0)
    parser.add_argument("--num-leaves", type=int, default=0)
    parser.add_argument("--num-super-spines", type=int, default=0)
    parser.add_argument("--num-sites", type=int, default=0)
    parser.add_argument(
        "--num-areas", type=int, default=1, help="Area count (default: 1)"
    )
    parser.add_argument(
        "--interfaces",
        type=int,
        default=None,
        help="Number of interfaces given to the tester; enables the distribution check",
    )
    parser.add_argument(
        "--wait",
        type=int,
        default=0,
        help="Seconds to wait for convergence before asserting (default: 0)",
    )
    parser.add_argument(
        "--poll-interval", type=int, default=5, help="Poll interval while waiting"
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON results")
    return parser.parse_args(argv)


async def _main_async(args: argparse.Namespace) -> int:
    expected = _resolve_expected(args)
    if args.wait > 0:
        established, adjacencies = await _wait_for_count(
            args.dut_host, args.dut_port, expected, args.wait, args.poll_interval
        )
    else:
        established, adjacencies = await _sample(args.dut_host, args.dut_port)

    results = _run_checks(established, adjacencies, expected, args.interfaces)
    _report(results, args.json)
    return 0 if all(ok for _, ok, _ in results) else 1


def main() -> int:
    args = _parse_args(sys.argv[1:])
    try:
        return asyncio.run(_main_async(args))
    except (OSError, ValueError) as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
