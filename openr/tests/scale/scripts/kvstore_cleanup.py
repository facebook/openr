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
/127 is point-to-point and deliberately not routable from off-box. Remote
connections require mTLS; the client certificate and CA are resolved through
the standard Open/R client options. The cleanup never retries with plaintext.

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
import ipaddress
import re
import sys
import time
from dataclasses import dataclass
from typing import Any, Awaitable, Callable, Iterable, Mapping, Sequence, TypeVar

from openr.py.openr.cli.utils.options import getDefaultOptions
from openr.py.openr.clients.openr_client import get_ssl_context
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


@dataclass(frozen=True)
class CleanupSummary:
    """Bounded cleanup result, suitable for runner-side diagnostics."""

    marked_by_host: Mapping[str, int]
    final_survivors_by_host: Mapping[str, tuple[str, ...]]
    host_failures: Mapping[str, str]


class CleanupError(Exception):
    """The cleanup could not be completed."""

    def __init__(self, message: str, summary: CleanupSummary | None = None) -> None:
        super().__init__(message)
        self.summary = summary


class _CleanupDeadlineExceeded(asyncio.TimeoutError):
    """The shared cleanup deadline elapsed before an operation could start."""


@dataclass(frozen=True)
class KeyRef:
    """Enough of a resident key to build a TTL update for it."""

    area: str
    key: str
    version: int
    originator: str
    ttl: int
    ttl_version: int


def _is_loopback_host(host: str) -> bool:
    if host.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


@dataclass(frozen=True)
class _HostSnapshot:
    self_node: str
    refs: tuple[KeyRef, ...]


@dataclass(frozen=True)
class _Target:
    label: str
    host: str
    port: int


_SCALE_NODE = r"(?:spine|leaf|control|eb-site)-\d+"
_SCALE_KEY_IDENTITY_REGEX = re.compile(
    rf"^(?:adj:{_SCALE_NODE}|prefix:{_SCALE_NODE}:.*|fakekeys\d+:{_SCALE_NODE})$"
)
_POLL_INTERVAL_SECONDS = 2.0
_MARK_REFRESH_FRACTION = 0.8
_T = TypeVar("_T")


def _client(host: str, port: int) -> Any:
    options = getDefaultOptions(host, timeout_ms=_TIMEOUT_MS)
    ssl_context = get_ssl_context(options)
    if ssl_context is None and not _is_loopback_host(host):
        raise CleanupError(
            f"mTLS is required for Open/R cleanup on {host}:{port}, but no "
            "client TLS configuration is available"
        )
    return get_client(
        OpenrCtrlCpp,
        host=host,
        port=port,
        timeout=_TIMEOUT_MS / 1000,
        client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
        ssl_context=ssl_context,
        ssl_timeout=_TIMEOUT_MS / 1000,
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


async def _expire_area_rejections(
    client: Any,
    area: str,
    refs: Sequence[KeyRef],
    ttl_ms: int,
    batch_size: int,
) -> dict[str, str]:
    rejected: dict[str, str] = {}
    for batch in _batched(refs, batch_size):
        params = KeySetParams(
            keyVals={ref.key: _ttl_update(ref, ttl_ms) for ref in batch}
        )
        result = await client.setKvStoreKeyValues(params, area)
        rejected.update(
            {
                key: getattr(reason, "name", str(reason))
                for key, reason in result.noMergeReasons.items()
            }
        )
    return rejected


async def expire_area(
    client: Any,
    area: str,
    refs: Sequence[KeyRef],
    ttl_ms: int,
    batch_size: int,
) -> dict[str, int]:
    """Send TTL updates for one area. Returns a no-merge reason histogram."""
    histogram: dict[str, int] = {}
    for reason in (
        await _expire_area_rejections(client, area, refs, ttl_ms, batch_size)
    ).values():
        histogram[reason] = histogram.get(reason, 0) + 1
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


def _validate_cleanup_inputs(
    hosts: Sequence[str], ttl_ms: int, wait_sec: int, batch_size: int
) -> tuple[_Target, ...]:
    if not hosts or any(
        not isinstance(token, str) or not token.strip() for token in hosts
    ):
        raise ValueError("hosts must be a nonempty sequence of nonempty strings")
    if ttl_ms <= 0:
        raise ValueError("ttl_ms must be a positive integer")
    if wait_sec <= 0:
        raise ValueError("wait_sec must be a positive integer")
    if batch_size <= 0:
        raise ValueError("batch_size must be a positive integer")
    if wait_sec * 1000 <= ttl_ms:
        raise ValueError("wait_sec * 1000 must exceed ttl_ms")
    targets = []
    for token in hosts:
        label = token.strip()
        host, port = parse_host(label)
        targets.append(_Target(label=label, host=host, port=port))
    return tuple(targets)


def _key_survivors(
    refs: Iterable[KeyRef], key_re: re.Pattern[str] | None
) -> tuple[str, ...]:
    return tuple(
        sorted(ref.key for ref in refs if key_re is None or key_re.search(ref.key))
    )


async def _scan_target(target: _Target, area: str | None) -> _HostSnapshot:
    async with _client(target.host, target.port) as client:
        self_node = await client.getMyNodeName()
        areas = await discover_areas(client, area)
        refs = await collect_keys(client, areas)
    return _HostSnapshot(self_node=self_node, refs=tuple(refs))


async def _unreconciled_rejections(
    client: Any,
    area: str,
    refs: Sequence[KeyRef],
    rejected: Mapping[str, str],
    ttl_ms: int,
) -> dict[str, str]:
    candidates = {
        key: reason for key, reason in rejected.items() if reason == "NO_NEED_TO_UPDATE"
    }
    unreconciled = {
        key: reason for key, reason in rejected.items() if reason != "NO_NEED_TO_UPDATE"
    }
    if not candidates:
        return unreconciled
    fresh = await client.getKvStoreHashFilteredArea(KeyDumpParams(), area)
    refs_by_key = {ref.key: ref for ref in refs}
    for key, reason in candidates.items():
        requested = refs_by_key.get(key)
        resident = fresh.keyVals.get(key)
        already_applied = (
            reason == "NO_NEED_TO_UPDATE"
            and requested is not None
            and resident is not None
            and resident.version == requested.version
            and resident.originatorId == requested.originator
            and resident.ttlVersion >= requested.ttl_version + 1
            and 0 < resident.ttl <= ttl_ms
        )
        if not already_applied:
            unreconciled[key] = reason
    return unreconciled


async def _mark_snapshot(
    target: _Target,
    snapshot: _HostSnapshot,
    refs: Sequence[KeyRef],
    ttl_ms: int,
    batch_size: int,
) -> None:
    if not refs:
        return
    by_area: dict[str, list[KeyRef]] = {}
    for ref in refs:
        by_area.setdefault(ref.area, []).append(ref)
    async with _client(target.host, target.port) as client:
        for current_area, area_refs in sorted(by_area.items()):
            rejected = await _expire_area_rejections(
                client, current_area, area_refs, ttl_ms, batch_size
            )
            unreconciled = await _unreconciled_rejections(
                client,
                current_area,
                area_refs,
                rejected,
                ttl_ms,
            )
            if unreconciled:
                histogram: dict[str, int] = {}
                for reason in unreconciled.values():
                    histogram[reason] = histogram.get(reason, 0) + 1
                raise CleanupError(
                    f"{target.label}: KvStore rejected {len(unreconciled)}/"
                    f"{len(area_refs)} TTL updates in area {current_area!r}: "
                    f"{histogram}"
                )


async def _mark_target(
    target: _Target,
    *,
    area: str | None,
    ttl_ms: int,
    batch_size: int,
    originator_re: re.Pattern[str] | None,
    key_re: re.Pattern[str] | None,
) -> tuple[_HostSnapshot, int]:
    snapshot = await _scan_target(target, area)
    selected = select_keys(
        snapshot.refs,
        snapshot.self_node,
        originator_re,
        key_re,
    )
    await _mark_snapshot(target, snapshot, selected, ttl_ms, batch_size)
    return snapshot, len(selected)


def _failure_text(error: BaseException) -> str:
    return f"{type(error).__name__}: {error}"


async def _gather_by_target(
    targets: Sequence[_Target],
    operation: Callable[[_Target], Awaitable[_T]],
    *,
    deadline: float | None = None,
) -> dict[str, _T | BaseException]:
    async def bounded(target: _Target) -> _T:
        if deadline is None:
            return await operation(target)
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            raise _CleanupDeadlineExceeded("cleanup deadline exceeded")
        return await asyncio.wait_for(operation(target), timeout=remaining)

    results = await asyncio.gather(
        *(bounded(target) for target in targets),
        return_exceptions=True,
    )
    return {target.label: result for target, result in zip(targets, results)}


def _cleanup_error(summary: CleanupSummary, wait_sec: int) -> CleanupError:
    failure_detail = ", ".join(
        f"{host}={failure}" for host, failure in sorted(summary.host_failures.items())
    )
    survivor_detail = ", ".join(
        f"{host}={keys[:20]}"
        for host, keys in sorted(summary.final_survivors_by_host.items())
    )
    details = "; ".join(
        detail
        for detail in (
            f"host_failures={{ {failure_detail} }}" if failure_detail else "",
            f"survivors={{ {survivor_detail} }}" if survivor_detail else "",
        )
        if detail
    )
    return CleanupError(
        f"scale-key cleanup did not reach stable zero within {wait_sec}s; {details}",
        summary,
    )


async def _expire_scale_keys_and_verify(  # noqa: C901
    hosts: Sequence[str],
    *,
    area: str | None,
    ttl_ms: int,
    wait_sec: int,
    batch_size: int,
    originator_re: re.Pattern[str] | None,
    key_re: re.Pattern[str] | None,
    verification_key_re: re.Pattern[str] | None,
) -> CleanupSummary:
    targets = _validate_cleanup_inputs(hosts, ttl_ms, wait_sec, batch_size)
    loop = asyncio.get_running_loop()
    deadline = loop.time() + wait_sec
    failures: dict[str, str] = {}
    marked_by_host = {target.label: 0 for target in targets}

    async def mark(target: _Target) -> tuple[_HostSnapshot, int]:
        return await _mark_target(
            target,
            area=area,
            ttl_ms=ttl_ms,
            batch_size=batch_size,
            originator_re=originator_re,
            key_re=key_re,
        )

    mark_started = loop.time()
    mark_results = await _gather_by_target(targets, mark, deadline=deadline)
    reachable: dict[str, _HostSnapshot] = {}
    for target in targets:
        result = mark_results[target.label]
        if isinstance(result, BaseException):
            failures[target.label] = _failure_text(result)
            continue
        snapshot, count = result
        reachable[target.label] = snapshot
        marked_by_host[target.label] = count

    ttl_seconds = ttl_ms / 1000
    if reachable and loop.time() - mark_started >= ttl_seconds * _MARK_REFRESH_FRACTION:
        refresh_results = await _gather_by_target(targets, mark, deadline=deadline)
        for target in targets:
            result = refresh_results[target.label]
            if isinstance(result, BaseException):
                failures[target.label] = _failure_text(result)
            else:
                failures.pop(target.label, None)
                snapshot, count = result
                reachable[target.label] = snapshot
                marked_by_host[target.label] += count

    remaining = deadline - loop.time()
    if remaining > 0 and any(marked_by_host.values()):
        await asyncio.sleep(min(ttl_seconds, remaining))

    stable_zero_scans = 0
    final_survivors: dict[str, tuple[str, ...]] = {}
    while loop.time() < deadline:
        scan_results = await _gather_by_target(
            targets,
            lambda target: _scan_target(target, area),
            deadline=deadline,
        )
        current_snapshots: dict[str, _HostSnapshot] = {}
        final_survivors = {}
        for target in targets:
            result = scan_results[target.label]
            if isinstance(result, BaseException):
                if (
                    isinstance(result, _CleanupDeadlineExceeded)
                    and target.label in failures
                ):
                    continue
                failures[target.label] = _failure_text(result)
                continue
            current_snapshots[target.label] = result
            failures.pop(target.label, None)
            survivors = _key_survivors(result.refs, verification_key_re)
            if survivors:
                final_survivors[target.label] = survivors

        if (
            len(current_snapshots) == len(targets)
            and not failures
            and not final_survivors
        ):
            stable_zero_scans += 1
            if stable_zero_scans == 2:
                summary = CleanupSummary(marked_by_host, {}, failures)
                return summary
            await asyncio.sleep(
                min(_POLL_INTERVAL_SECONDS, max(0, deadline - loop.time()))
            )
            continue

        stable_zero_scans = 0
        refreshable = False

        async def refresh(
            target: _Target,
            snapshots: Mapping[str, _HostSnapshot] = current_snapshots,
        ) -> None:
            nonlocal refreshable
            snapshot = snapshots.get(target.label)
            if snapshot is None:
                return
            selected = select_keys(
                snapshot.refs,
                snapshot.self_node,
                originator_re,
                key_re,
            )
            if selected:
                refreshable = True
                await _mark_snapshot(target, snapshot, selected, ttl_ms, batch_size)
                marked_by_host[target.label] += len(selected)

        refresh_results = await _gather_by_target(targets, refresh, deadline=deadline)
        for target in targets:
            result = refresh_results[target.label]
            if isinstance(result, BaseException) and target.label in current_snapshots:
                failures[target.label] = _failure_text(result)
        if final_survivors and not refreshable:
            break
        remaining = deadline - loop.time()
        if remaining <= 0:
            break
        await asyncio.sleep(min(ttl_seconds, remaining))

    summary = CleanupSummary(marked_by_host, final_survivors, failures)
    raise _cleanup_error(summary, wait_sec)


async def expire_scale_keys_and_verify(
    hosts: Sequence[str],
    *,
    area: str | None = None,
    ttl_ms: int = 30_000,
    wait_sec: int = 90,
    batch_size: int = 500,
) -> CleanupSummary:
    """Expire trusted scale Values and prove stable key-only zero on every host."""
    return await _expire_scale_keys_and_verify(
        hosts,
        area=area,
        ttl_ms=ttl_ms,
        wait_sec=wait_sec,
        batch_size=batch_size,
        originator_re=re.compile(_DEFAULT_ORIGINATOR_REGEX),
        key_re=re.compile(_DEFAULT_KEY_REGEX),
        verification_key_re=_SCALE_KEY_IDENTITY_REGEX,
    )


async def run(args: argparse.Namespace) -> int:
    hosts = tuple(token.strip() for token in args.hosts.split(",") if token.strip())
    if not hosts:
        raise CleanupError("--hosts is empty")
    originator_re = re.compile(args.originator_regex) if args.originator_regex else None
    key_re = re.compile(args.key_regex) if args.key_regex else None
    if not args.apply:
        targets = _validate_cleanup_inputs(
            hosts, args.ttl_ms, args.wait_sec, args.batch_size
        )
        results = await _gather_by_target(
            targets,
            lambda target: mark_host(
                target.host,
                target.port,
                args,
                originator_re,
                key_re,
            ),
        )
        failures = {
            host: _failure_text(result)
            for host, result in results.items()
            if isinstance(result, BaseException)
        }
        if failures:
            raise CleanupError(f"dry-run host failures: {failures}")
        selected_by_host = {
            host: result
            for host, result in results.items()
            if not isinstance(result, BaseException)
        }
        total = sum(len(refs) for refs in selected_by_host.values())
        print(f"\nDRY RUN: {total} key(s) would be expired. Re-run with --apply.")
        return 0
    try:
        summary = await _expire_scale_keys_and_verify(
            hosts,
            area=args.area,
            ttl_ms=args.ttl_ms,
            wait_sec=args.wait_sec,
            batch_size=args.batch_size,
            originator_re=originator_re,
            key_re=key_re,
            verification_key_re=(
                _SCALE_KEY_IDENTITY_REGEX
                if args.key_regex == _DEFAULT_KEY_REGEX
                else key_re
            ),
        )
    except CleanupError as error:
        print(f"\nFAILED: {error}")
        return 1
    total = sum(summary.marked_by_host.values())
    print(f"\nOK: {total} key TTL update(s) sent; stable zero verified.")
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
