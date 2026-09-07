# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


"""Expire scale-tester keys from one or more Open/R KvStores.

Why this exists
---------------
The scale tester injects every key with ``Constants::kTtlInfinity`` and never
withdraws anything, so a finished (or crashed) run leaves its whole synthetic
fabric resident on the DUT forever.

Restarting Open/R is NOT a fix. KvStore is pure in-memory and does start empty,
but it immediately full-syncs from its peers -- and on a 1-DUT + 1-helper rig the
peer is a mirror holding the same immortal keys, which flood straight back.
Sequential restarts never converge: restart the DUT and the helper re-teaches it,
restart the helper and the DUT re-teaches back. Only a simultaneous restart of
every node in the area would work, and that is rarely practical.

There is no generic "delete key" API. ``KvStoreService`` exposes only
``unsetSelfOriginatedKey``, which applies to keys the node itself originated;
injected keys are originated by the *simulated* nodes, so it does not reach
them. (It would also copy the resident TTL, leaving an immortal empty tombstone.)

The supported mechanism is to make the keys mortal and let KvStore's own expiry
delete them. For each key we send a **value-less** ``Value`` carrying the
resident ``version`` and ``originatorId``, ``ttlVersion + 1``, and a short
``ttl``. KvStore takes the ``UPDATE_TTL_NEEDED`` path, ``updateKvStoreTtl()``
rewrites ttl/ttlVersion in place, ``updateTtlCountdownQueue()`` enqueues the key
because the ttl is no longer infinite, and ``cleanupTtlCountdownQueue()`` erases
it and publishes it on ``expiredKeys``.

Properties that make this safe:
  * No payload is sent, so requests stay small -- this path is not subject to the
    control-plane policing that resets bulk ``adj:`` injection over mgmt.
  * The stored value is untouched, so nothing is corrupted while the key lives.
  * Decision explicitly skips value-less updates (Decision.cpp:780), so no SPF
    churn happens until the key actually expires.
  * Self-originated keys are skipped by KvStore's own expiry logic, so a node can
    never be made to expire its own advertisements.

Safety
------
Dry-run by default; ``--apply`` is required to send anything. Keys originated by
the node being cleaned are excluded unconditionally. The default originator
filter matches only scale-tester synthetic node names
(``spine-N`` / ``leaf-N`` / ``control-N`` / ``eb-site-N``), so a real peer's
genuine keys are left alone unless you widen it deliberately.

All hosts are marked before any waiting, and ``--ttl-ms`` defaults to 30s rather
than something short. Both matter: mutually-peered nodes re-teach each other on
any full sync, so the goal is for every node to be marked well before the first
one starts expiring, making expiry near-simultaneous across the area. A short TTL
would let the first host expire while later hosts are still being marked, leaving
a window in which a peer flap re-teaches the keys.

Connectivity
------------
Use the mgmt addresses or hostnames from a devserver -- the eb02 <-> eb04 inband
/127 is point-to-point and deliberately not routable from off-box.

Examples
--------
  # See what would be expired on both lab boxes (no writes)
  kvstore_cleanup --hosts eb02.lab.ash6,eb04.lab.ash6

  # Actually expire the synthetic fabric
  kvstore_cleanup --hosts eb02.lab.ash6,eb04.lab.ash6 --apply

  # Also drop the filler keys, and widen to a custom generator
  kvstore_cleanup --hosts eb04.lab.ash6 --apply \\
      --key-regex '^(adj|prefix|fakekeys[0-9]+):' \\
      --originator-regex '^(spine|leaf)-[0-9]+$'
"""

from __future__ import annotations

import argparse
import asyncio
import re
import sys
import time
from dataclasses import dataclass
from typing import Any, Iterable, Mapping, Sequence

from openr.thrift.KvStore.thrift_types import KeyDumpParams, KeySetParams, Value
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp
from thrift.python.client import ClientType, get_client

_TIMEOUT_MS = 60 * 1000
_DEFAULT_PORT = 2018

# Node names emitted by BbfTopologyGenerator::createBbfSimple. Anchored so a real
# device whose name merely contains "leaf" is never matched.
_DEFAULT_ORIGINATOR_REGEX = r"^(spine|leaf|control|eb-site)-\d+$"

# Key classes the scale tester injects.
_DEFAULT_KEY_REGEX = r"^(adj:|prefix:|fakekeys\d+:)"


class CleanupError(Exception):
    """The cleanup could not be completed."""


@dataclass(frozen=True)
class KeyRef:
    """Enough of a resident key to build a TTL update for it."""

    area: str
    key: str
    version: int
    originator: str
    ttl: int
    ttl_version: int


def _client(host: str, port: int) -> Any:
    return get_client(
        OpenrCtrlCpp,
        host=host,
        port=port,
        timeout=_TIMEOUT_MS,
        client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
    )


def parse_host(token: str) -> tuple[str, int]:
    """Parse ``host`` or ``host:port``. Bracket IPv6 literals to use a port."""
    if token.startswith("["):
        host, _, rest = token[1:].partition("]")
        return host, int(rest.lstrip(":")) if rest.lstrip(":") else _DEFAULT_PORT
    # A bare IPv6 literal has several colons; only treat a single one as a port.
    if token.count(":") == 1:
        host, _, port = token.partition(":")
        return host, int(port)
    return token, _DEFAULT_PORT


async def discover_areas(client: Any, area: str | None) -> list[str]:
    if area:
        return [area]
    summaries = await client.getKvStoreAreaSummary(set())
    return sorted(s.area for s in summaries)


async def collect_keys(client: Any, areas: Sequence[str]) -> list[KeyRef]:
    """Dump key metadata for every area.

    Uses the hash-filtered dump: it returns version/originatorId/ttl/ttlVersion
    without any serialized values, which is ~200 KB rather than ~25 MB for a
    4k-key fabric.
    """
    refs: list[KeyRef] = []
    for area in areas:
        pub = await client.getKvStoreHashFilteredArea(KeyDumpParams(), area)
        refs.extend(
            KeyRef(
                area=area,
                key=key,
                version=val.version,
                originator=val.originatorId,
                ttl=val.ttl,
                ttl_version=val.ttlVersion,
            )
            for key, val in pub.keyVals.items()
        )
    return refs


def select_keys(
    refs: Iterable[KeyRef],
    self_node: str,
    originator_re: re.Pattern[str] | None,
    key_re: re.Pattern[str] | None,
) -> list[KeyRef]:
    """Filter to the keys we are willing to expire.

    Excluding ``self_node`` is not overridable. KvStore refuses to expire its own
    self-originated keys anyway, so including them would be a silent no-op that
    only makes the dry-run output lie.
    """
    selected = []
    for ref in refs:
        if ref.originator == self_node:
            continue
        if originator_re is not None and not originator_re.search(ref.originator):
            continue
        if key_re is not None and not key_re.search(ref.key):
            continue
        selected.append(ref)
    return selected


def _ttl_update(ref: KeyRef, ttl_ms: int) -> Value:
    """A value-less TTL update: same version/originator, bumped ttlVersion.

    Omitting ``value`` is what makes this a TTL update rather than a value
    rewrite. The version and originatorId MUST match the resident copy -- a
    mismatch is treated as an inconsistency and the update is dropped.
    """
    return Value(
        version=ref.version,
        originatorId=ref.originator,
        ttl=ttl_ms,
        ttlVersion=ref.ttl_version + 1,
    )


def _batched(refs: Sequence[KeyRef], size: int) -> Iterable[Sequence[KeyRef]]:
    for start in range(0, len(refs), size):
        yield refs[start : start + size]


async def expire_area(
    client: Any,
    area: str,
    refs: Sequence[KeyRef],
    ttl_ms: int,
    batch_size: int,
) -> dict[str, int]:
    """Send TTL updates for one area. Returns a no-merge reason histogram."""
    histogram: dict[str, int] = {}
    for batch in _batched(refs, batch_size):
        params = KeySetParams(
            keyVals={ref.key: _ttl_update(ref, ttl_ms) for ref in batch}
        )
        result = await client.setKvStoreKeyValues(params, area)
        for reason in result.noMergeReasons.values():
            name = getattr(reason, "name", str(reason))
            histogram[name] = histogram.get(name, 0) + 1
    return histogram


def summarize(refs: Sequence[KeyRef]) -> str:
    """Per-area, per-key-class counts plus a couple of sample keys."""
    if not refs:
        return "    (none)"
    by_area: dict[str, dict[str, int]] = {}
    for ref in refs:
        klass = ref.key.split(":", 1)[0]
        by_area.setdefault(ref.area, {}).setdefault(klass, 0)
        by_area[ref.area][klass] += 1

    lines = []
    for area in sorted(by_area):
        classes = ", ".join(
            f"{klass}={count}" for klass, count in sorted(by_area[area].items())
        )
        lines.append(f"    area {area!r}: {classes}")
    samples = sorted(ref.key for ref in refs)[:3]
    lines.append(f"    e.g. {', '.join(samples)}")
    return "\n".join(lines)


async def mark_host(
    host: str,
    port: int,
    args: argparse.Namespace,
    originator_re: re.Pattern[str] | None,
    key_re: re.Pattern[str] | None,
) -> list[KeyRef]:
    """Dump, filter, report, and (with --apply) send TTL updates for one host.

    Phase timings are printed because the interesting comparison (running this
    on-device over inband vs. from the test runner over mgmt) is invisible in
    the end-to-end wall clock -- that is dominated by the deliberate TTL sleep,
    which is identical either way. Connect/dump/mark are the parts that differ.
    """
    t_start = time.monotonic()
    async with _client(host, port) as client:
        self_node = await client.getMyNodeName()
        areas = await discover_areas(client, args.area)
        t_connected = time.monotonic()

        all_refs = await collect_keys(client, areas)
        t_dumped = time.monotonic()
        selected = select_keys(all_refs, self_node, originator_re, key_re)

        print(f"\n{host}:{port}  (node {self_node})")
        print(f"  areas: {', '.join(areas) or '(none)'}")
        print(f"  resident keys: {len(all_refs)}")
        print(f"  selected for expiry: {len(selected)}")
        print(summarize(selected))

        if not args.apply or not selected:
            print(
                f"  timing: connect {t_connected - t_start:.2f}s, "
                f"dump {t_dumped - t_connected:.2f}s"
            )
            return selected

        by_area: dict[str, list[KeyRef]] = {}
        for ref in selected:
            by_area.setdefault(ref.area, []).append(ref)
        for area, refs in sorted(by_area.items()):
            histogram = await expire_area(
                client, area, refs, args.ttl_ms, args.batch_size
            )
            rejected = sum(histogram.values())
            detail = (
                " (" + ", ".join(f"{k}={v}" for k, v in sorted(histogram.items())) + ")"
                if histogram
                else ""
            )
            print(
                f"  area {area!r}: ttl set on {len(refs) - rejected}/{len(refs)}"
                f"{detail}"
            )
        t_marked = time.monotonic()
        rpcs = sum(
            (len(refs) + args.batch_size - 1) // args.batch_size
            for refs in by_area.values()
        )
        print(
            f"  timing: connect {t_connected - t_start:.2f}s, "
            f"dump {t_dumped - t_connected:.2f}s, "
            f"mark {t_marked - t_dumped:.2f}s ({rpcs} RPC(s)), "
            f"total-excluding-ttl-wait {t_marked - t_start:.2f}s"
        )
        return selected


async def verify_host(
    host: str,
    port: int,
    args: argparse.Namespace,
    expected_gone: Mapping[str, set[str]],
) -> int:
    """Count keys that were marked but are still resident. 0 means fully clean."""
    async with _client(host, port) as client:
        areas = await discover_areas(client, args.area)
        survivors = 0
        for area in areas:
            pub = await client.getKvStoreHashFilteredArea(KeyDumpParams(), area)
            still_there = expected_gone.get(area, set()) & set(pub.keyVals)
            survivors += len(still_there)
            if still_there:
                sample = ", ".join(sorted(still_there)[:3])
                print(
                    f"  {host}: area {area!r} still holds {len(still_there)} "
                    f"key(s), e.g. {sample}"
                )
    return survivors


async def run(args: argparse.Namespace) -> int:
    originator_re = re.compile(args.originator_regex) if args.originator_regex else None
    key_re = re.compile(args.key_regex) if args.key_regex else None
    targets = [parse_host(token) for token in args.hosts.split(",") if token.strip()]
    if not targets:
        raise CleanupError("--hosts is empty")

    # Phase 1: mark every host BEFORE waiting. Peered nodes re-teach each other
    # on any full sync, so every host must carry a short TTL before the first one
    # expires -- otherwise a peer flap during the gap undoes the cleanup.
    marked: dict[tuple[str, int], list[KeyRef]] = {}
    for host, port in targets:
        marked[(host, port)] = await mark_host(host, port, args, originator_re, key_re)

    total = sum(len(refs) for refs in marked.values())
    if not args.apply:
        print(f"\nDRY RUN: {total} key(s) would be expired. Re-run with --apply.")
        return 0
    if total == 0:
        print("\nNothing to do.")
        return 0

    # Phase 2: sleep out the TTL before polling. Each poll is a full hash dump of
    # every host, so polling during the countdown just loads the boxes for
    # answers we already know.
    settle = args.ttl_ms / 1000
    print(f"\nMarked. Sleeping {settle:.0f}s for the TTL to run out...")
    await asyncio.sleep(settle)

    deadline = max(2, args.wait_sec - settle)
    print(f"Polling for up to {deadline:.0f}s...")
    survivors = total
    while True:
        survivors = 0
        for (host, port), refs in marked.items():
            expected: dict[str, set[str]] = {}
            for ref in refs:
                expected.setdefault(ref.area, set()).add(ref.key)
            survivors += await verify_host(host, port, args, expected)
        if survivors == 0 or deadline <= 0:
            break
        await asyncio.sleep(2)
        deadline -= 2

    if survivors:
        print(f"\nFAILED: {survivors} key(s) still resident after {args.wait_sec}s.")
        return 1
    print(f"\nOK: {total} key(s) expired.")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="kvstore_cleanup",
        description="Expire scale-tester keys from Open/R KvStores.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--hosts",
        default="eb02.lab.ash6,eb04.lab.ash6",
        help="Comma-separated host[:port] list (default: the ash6 lab pair). "
        "Bracket IPv6 literals to give a port, e.g. [2401:db00::1]:2018.",
    )
    parser.add_argument(
        "--area", default=None, help="Restrict to one area (default: all areas)."
    )
    parser.add_argument(
        "--originator-regex",
        default=_DEFAULT_ORIGINATOR_REGEX,
        help="Only expire keys whose originatorId matches. Pass '' to disable "
        f"(default: {_DEFAULT_ORIGINATOR_REGEX!r}).",
    )
    parser.add_argument(
        "--key-regex",
        default=_DEFAULT_KEY_REGEX,
        help="Only expire keys whose name matches. Pass '' to disable "
        f"(default: {_DEFAULT_KEY_REGEX!r}).",
    )
    parser.add_argument(
        "--ttl-ms",
        type=int,
        default=30000,
        help="TTL to set, in ms (default: 30000). Must be > 0. Keep it well "
        "above the time it takes to mark every host, so mutually-peered nodes "
        "expire together and cannot re-teach each other.",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=500,
        help="Keys per setKvStoreKeyValues call (default: 500).",
    )
    parser.add_argument(
        "--wait-sec",
        type=int,
        default=90,
        help="How long to poll for expiry after marking (default: 90). Must "
        "exceed --ttl-ms or the poll gives up before the keys are due.",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Actually send the TTL updates. Without it this is a dry run.",
    )
    args = parser.parse_args(argv)
    if args.ttl_ms <= 0:
        parser.error("--ttl-ms must be > 0 (0 and negatives are not valid TTLs)")
    if args.wait_sec * 1000 <= args.ttl_ms:
        parser.error(
            f"--wait-sec ({args.wait_sec}s) must exceed --ttl-ms "
            f"({args.ttl_ms}ms) or the poll gives up before the keys are due"
        )
    return args


def main() -> int:
    args = parse_args(sys.argv[1:])
    try:
        return asyncio.run(run(args))
    except Exception as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
