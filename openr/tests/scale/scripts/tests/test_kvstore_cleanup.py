# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import argparse
import asyncio
import re
import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock, patch

from later.unittest import TestCase
from openr.tests.scale.scripts import kvstore_cleanup
from openr.tests.scale.scripts.kvstore_cleanup import (
    _HostSnapshot,
    _mark_snapshot,
    _Target,
    _ttl_update,
    CleanupError,
    expire_scale_keys_and_verify,
    KeyRef,
    run,
    select_keys,
    verify_host,
)
from openr.thrift.KvStore.thrift_types import (
    KvStoreNoMergeReason,
    SetKeyValsResult,
    Value,
)


_MODULE = "openr.tests.scale.scripts.kvstore_cleanup"


class ClientTest(unittest.TestCase):
    @patch.object(kvstore_cleanup, "get_client")
    @patch.object(kvstore_cleanup, "get_ssl_context")
    @patch.object(kvstore_cleanup, "getDefaultOptions")
    def test_client_requires_tls_and_passes_context(
        self,
        get_default_options: MagicMock,
        get_ssl_context: MagicMock,
        get_client: MagicMock,
    ) -> None:
        options = object()
        ssl_context = object()
        thrift_client = object()
        get_default_options.return_value = options
        get_ssl_context.return_value = ssl_context
        get_client.return_value = thrift_client

        self.assertIs(thrift_client, kvstore_cleanup._client("eb02.lab.ash6", 2018))

        get_default_options.assert_called_once_with("eb02.lab.ash6", timeout_ms=60_000)
        get_ssl_context.assert_called_once_with(options)
        get_client.assert_called_once_with(
            kvstore_cleanup.OpenrCtrlCpp,
            host="eb02.lab.ash6",
            port=2018,
            timeout=60.0,
            client_type=kvstore_cleanup.ClientType.THRIFT_ROCKET_CLIENT_TYPE,
            ssl_context=ssl_context,
            ssl_timeout=60.0,
        )

    @patch.object(kvstore_cleanup, "get_client")
    @patch.object(kvstore_cleanup, "get_ssl_context", return_value=None)
    @patch.object(kvstore_cleanup, "getDefaultOptions")
    def test_client_rejects_missing_tls_configuration(
        self,
        get_default_options: MagicMock,
        get_ssl_context: MagicMock,
        get_client: MagicMock,
    ) -> None:
        with self.assertRaisesRegex(
            kvstore_cleanup.CleanupError,
            "mTLS is required.*no client TLS configuration",
        ):
            kvstore_cleanup._client("eb02.lab.ash6", 2018)

        get_default_options.assert_called_once_with("eb02.lab.ash6", timeout_ms=60_000)
        get_ssl_context.assert_called_once_with(get_default_options.return_value)
        get_client.assert_not_called()

    @patch.object(kvstore_cleanup, "get_client")
    @patch.object(kvstore_cleanup, "get_ssl_context", return_value=None)
    @patch.object(kvstore_cleanup, "getDefaultOptions")
    def test_client_allows_plaintext_on_loopback(
        self,
        get_default_options: MagicMock,
        get_ssl_context: MagicMock,
        get_client: MagicMock,
    ) -> None:
        thrift_client = object()
        get_client.return_value = thrift_client

        self.assertIs(thrift_client, kvstore_cleanup._client("::1", 2018))

        get_ssl_context.assert_called_once_with(get_default_options.return_value)
        self.assertIsNone(get_client.call_args.kwargs["ssl_context"])

    @patch.object(kvstore_cleanup, "get_client")
    @patch.object(kvstore_cleanup, "get_ssl_context")
    @patch.object(kvstore_cleanup, "getDefaultOptions")
    def test_client_does_not_retry_without_tls(
        self,
        get_default_options: MagicMock,
        get_ssl_context: MagicMock,
        get_client: MagicMock,
    ) -> None:
        connection_error = RuntimeError("TLS handshake failed")
        get_ssl_context.return_value = object()
        get_client.side_effect = connection_error

        with self.assertRaisesRegex(RuntimeError, "TLS handshake failed"):
            kvstore_cleanup._client("eb02.lab.ash6", 2018)

        get_client.assert_called_once()


def _args(
    hosts: str = "first.example,second.example", **overrides: object
) -> argparse.Namespace:
    values: dict[str, object] = {
        "hosts": hosts,
        "area": "0",
        "originator_regex": r"^(spine|leaf|control|eb-site)-\d+$",
        "key_regex": r"^(adj:|prefix:|fakekeys\d+:)",
        "ttl_ms": 30_000,
        "batch_size": 500,
        "wait_sec": 90,
        "apply": True,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class _ClientContext:
    def __init__(self, client: object) -> None:
        self.client = client

    async def __aenter__(self) -> object:
        return self.client

    async def __aexit__(self, *args: object) -> None:
        return None


class KvStoreCleanupCharacterizationTest(TestCase):
    def test_selects_only_synthetic_originators_and_never_self(self) -> None:
        refs = [
            KeyRef("0", "adj:leaf-1", 1, "leaf-1", -1, 0),
            KeyRef("0", "prefix:leaf-2:[fc00::1/128]", 1, "leaf-2", -1, 0),
            KeyRef("0", "adj:dut", 1, "dut", -1, 0),
            KeyRef("0", "prefix:real:[2001:db8::/64]", 1, "real", -1, 0),
            KeyRef("0", "unrelated", 1, "leaf-3", -1, 0),
        ]

        selected = select_keys(
            refs,
            "dut",
            re.compile(r"^(spine|leaf|control|eb-site)-\d+$"),
            re.compile(r"^(adj:|prefix:|fakekeys\d+:)"),
        )

        self.assertEqual(
            ["adj:leaf-1", "prefix:leaf-2:[fc00::1/128]"],
            [ref.key for ref in selected],
        )

    def test_ttl_update_is_valueless_and_preserves_resident_identity(self) -> None:
        update = _ttl_update(
            KeyRef("0", "adj:leaf-1", 7, "leaf-1", -2_147_483_648, 13),
            30_000,
        )

        self.assertEqual(7, update.version)
        self.assertEqual("leaf-1", update.originatorId)
        self.assertEqual(30_000, update.ttl)
        self.assertEqual(14, update.ttlVersion)
        self.assertIsNone(update.value)

    async def test_marks_every_host_before_the_first_ttl_wait(self) -> None:
        marked_hosts: list[str] = []

        async def mark_target(
            target: _Target, **_kwargs: object
        ) -> tuple[_HostSnapshot, int]:
            host = target.label
            marked_hosts.append(host)
            return _HostSnapshot(self_node="dut", refs=()), 1

        async def scan_target(_target: _Target, _area: str | None) -> _HostSnapshot:
            return _HostSnapshot(self_node="dut", refs=())

        sleep_calls = 0

        async def sleep(_seconds: float) -> None:
            nonlocal sleep_calls
            if sleep_calls == 0:
                self.assertCountEqual(["first.example", "second.example"], marked_hosts)
            sleep_calls += 1

        with (
            patch(f"{_MODULE}._mark_target", side_effect=mark_target),
            patch(f"{_MODULE}._scan_target", side_effect=scan_target),
            patch(f"{_MODULE}.asyncio.sleep", side_effect=sleep),
        ):
            result = await run(_args())

        self.assertEqual(0, result)

    async def test_captured_key_verification_ignores_a_new_synthetic_survivor(
        self,
    ) -> None:
        client = AsyncMock()
        client.getKvStoreHashFilteredArea.return_value = SimpleNamespace(
            keyVals={"adj:leaf-99": object()}
        )

        with patch(f"{_MODULE}._client", return_value=_ClientContext(client)):
            survivors = await verify_host(
                "dut.example",
                2018,
                _args(hosts="dut.example"),
                {"0": {"adj:leaf-1"}},
            )

        self.assertEqual(0, survivors)

    async def test_empty_baseline_succeeds_after_stable_zero_rescans(self) -> None:
        snapshot = SimpleNamespace(self_node="dut", refs=())
        sleep = AsyncMock()
        with (
            patch(f"{_MODULE}._mark_target", AsyncMock(return_value=(snapshot, 0))),
            patch(f"{_MODULE}._scan_target", AsyncMock(return_value=snapshot)),
            patch(f"{_MODULE}.asyncio.sleep", sleep),
        ):
            result = await run(_args())

        self.assertEqual(0, result)
        self.assertGreaterEqual(sleep.await_count, 1)


class KvStoreCleanupApiTest(TestCase):
    async def test_key_only_rescan_reports_ineligible_originator_survivor(self) -> None:
        trusted = KeyRef("0", "adj:leaf-1", 1, "leaf-1", -1, 0)
        protected = KeyRef("0", "adj:leaf-99", 1, "dut", -1, 0)
        snapshots = AsyncMock(
            side_effect=(
                SimpleNamespace(self_node="dut", refs=(trusted, protected)),
                SimpleNamespace(self_node="dut", refs=(protected,)),
            )
        )
        mark = AsyncMock()

        with (
            patch(f"{_MODULE}._scan_target", snapshots),
            patch(f"{_MODULE}._mark_snapshot", mark),
            patch(f"{_MODULE}.asyncio.sleep", AsyncMock()),
        ):
            with self.assertRaises(CleanupError) as context:
                await expire_scale_keys_and_verify(
                    ["dut.example"],
                    area="0",
                    ttl_ms=1_000,
                    wait_sec=2,
                )

        message = str(context.exception)
        self.assertIn("dut.example", message)
        self.assertIn("adj:leaf-99", message)
        self.assertEqual(
            ["adj:leaf-1"],
            [ref.key for ref in mark.await_args_list[0].args[2]],
        )

    async def test_already_applied_same_ttl_version_is_reconciled(self) -> None:
        ref = KeyRef("0", "adj:leaf-1", 7, "leaf-1", -1, 13)
        client = AsyncMock()
        client.setKvStoreKeyValues.return_value = SetKeyValsResult(
            noMergeReasons={ref.key: KvStoreNoMergeReason.NO_NEED_TO_UPDATE}
        )
        client.getKvStoreHashFilteredArea.return_value = SimpleNamespace(
            keyVals={
                ref.key: SimpleNamespace(
                    version=7,
                    originatorId="leaf-1",
                    ttl=29_999,
                    ttlVersion=14,
                )
            }
        )

        with patch(f"{_MODULE}._client", return_value=_ClientContext(client)):
            await _mark_snapshot(
                _Target("dut.example", "dut.example", 2018),
                _HostSnapshot("dut", (ref,)),
                [ref],
                ttl_ms=30_000,
                batch_size=500,
            )

        client.getKvStoreHashFilteredArea.assert_awaited_once()

    async def test_actual_rejected_ttl_mutation_still_fails(self) -> None:
        ref = KeyRef("0", "adj:leaf-1", 7, "leaf-1", -1, 13)
        client = AsyncMock()
        client.setKvStoreKeyValues.return_value = SetKeyValsResult(
            noMergeReasons={ref.key: KvStoreNoMergeReason.OLD_VERSION}
        )
        client.getKvStoreHashFilteredArea.return_value = SimpleNamespace(
            keyVals={
                ref.key: SimpleNamespace(
                    version=7,
                    originatorId="leaf-1",
                    ttl=29_999,
                    ttlVersion=14,
                )
            }
        )

        with patch(f"{_MODULE}._client", return_value=_ClientContext(client)):
            with self.assertRaisesRegex(CleanupError, "OLD_VERSION"):
                await _mark_snapshot(
                    _Target("dut.example", "dut.example", 2018),
                    _HostSnapshot("dut", (ref,)),
                    [ref],
                    ttl_ms=30_000,
                    batch_size=500,
                )

        client.getKvStoreHashFilteredArea.assert_not_awaited()

    async def test_no_need_with_absent_fresh_metadata_still_fails(self) -> None:
        ref = KeyRef("0", "adj:leaf-1", 7, "leaf-1", -1, 13)
        client = AsyncMock()
        client.setKvStoreKeyValues.return_value = SetKeyValsResult(
            noMergeReasons={ref.key: KvStoreNoMergeReason.NO_NEED_TO_UPDATE}
        )
        client.getKvStoreHashFilteredArea.return_value = SimpleNamespace(keyVals={})

        with patch(f"{_MODULE}._client", return_value=_ClientContext(client)):
            with self.assertRaisesRegex(CleanupError, "NO_NEED_TO_UPDATE"):
                await _mark_snapshot(
                    _Target("dut.example", "dut.example", 2018),
                    _HostSnapshot("dut", (ref,)),
                    [ref],
                    ttl_ms=30_000,
                    batch_size=500,
                )

    async def test_no_need_without_advanced_ttl_version_still_fails(self) -> None:
        ref = KeyRef("0", "adj:leaf-1", 7, "leaf-1", -1, 13)
        client = AsyncMock()
        client.setKvStoreKeyValues.return_value = SetKeyValsResult(
            noMergeReasons={ref.key: KvStoreNoMergeReason.NO_NEED_TO_UPDATE}
        )
        client.getKvStoreHashFilteredArea.return_value = SimpleNamespace(
            keyVals={
                ref.key: Value(
                    version=7,
                    originatorId="leaf-1",
                    ttl=29_999,
                    ttlVersion=13,
                )
            }
        )

        with patch(f"{_MODULE}._client", return_value=_ClientContext(client)):
            with self.assertRaisesRegex(CleanupError, "NO_NEED_TO_UPDATE"):
                await _mark_snapshot(
                    _Target("dut.example", "dut.example", 2018),
                    _HostSnapshot("dut", (ref,)),
                    [ref],
                    ttl_ms=30_000,
                    batch_size=500,
                )

    async def test_host_failure_does_not_skip_peer_attempts(self) -> None:
        calls: list[str] = []

        async def scan(target: _Target, _area: str | None) -> _HostSnapshot:
            calls.append(target.label)
            if target.label == "first.example":
                raise RuntimeError("unreachable")
            return _HostSnapshot(self_node="dut", refs=())

        with patch(f"{_MODULE}._scan_target", side_effect=scan):
            with self.assertRaisesRegex(CleanupError, "first.example.*unreachable"):
                await expire_scale_keys_and_verify(
                    ["first.example", "second.example"],
                    area="0",
                    ttl_ms=1,
                    wait_sec=1,
                )

        self.assertIn("second.example", calls)

    async def test_transient_scan_failure_can_recover(self) -> None:
        snapshot = _HostSnapshot(self_node="dut", refs=())
        scans = AsyncMock(
            side_effect=(snapshot, RuntimeError("transient"), snapshot, snapshot)
        )
        with (
            patch(f"{_MODULE}._scan_target", scans),
            patch(f"{_MODULE}.asyncio.sleep", AsyncMock()),
        ):
            summary = await expire_scale_keys_and_verify(
                ["dut.example"], area="0", ttl_ms=1_000, wait_sec=2
            )

        self.assertEqual({}, dict(summary.host_failures))
        self.assertEqual(4, scans.await_count)

    async def test_requires_two_fresh_zero_rescans(self) -> None:
        snapshot = SimpleNamespace(self_node="dut", refs=())
        scans = AsyncMock(side_effect=(snapshot, snapshot, snapshot))
        sleep = AsyncMock()

        with (
            patch(f"{_MODULE}._scan_target", scans),
            patch(f"{_MODULE}.asyncio.sleep", sleep),
        ):
            summary = await expire_scale_keys_and_verify(
                ["dut.example"],
                area="0",
                ttl_ms=1_000,
                wait_sec=2,
            )

        self.assertEqual({"dut.example": 0}, dict(summary.marked_by_host))
        self.assertEqual({}, dict(summary.final_survivors_by_host))
        self.assertEqual(3, scans.await_count)
        sleep.assert_awaited_once()

    async def test_programmatic_deadline_bounds_a_hung_host(self) -> None:
        started = asyncio.Event()

        async def hang(_target: object, _area: str | None) -> object:
            started.set()
            await asyncio.Event().wait()
            raise AssertionError("unreachable")

        with patch(f"{_MODULE}._scan_target", side_effect=hang):
            with self.assertRaisesRegex(CleanupError, "dut.example.*TimeoutError"):
                await expire_scale_keys_and_verify(
                    ["dut.example"], ttl_ms=10, wait_sec=1
                )

        self.assertTrue(started.is_set())

    async def test_cli_default_verification_ignores_legitimate_dut_keys(self) -> None:
        snapshot = SimpleNamespace(
            self_node="dut",
            refs=(
                KeyRef("0", "adj:dut", 1, "dut", -1, 0),
                KeyRef("0", "prefix:dut:[2001:db8::/64]", 1, "dut", -1, 0),
            ),
        )

        with (
            patch(f"{_MODULE}._scan_target", AsyncMock(return_value=snapshot)),
            patch(f"{_MODULE}.asyncio.sleep", AsyncMock()),
        ):
            result = await run(_args())

        self.assertEqual(0, result)

    async def test_cli_custom_key_filter_is_used_for_key_only_verification(
        self,
    ) -> None:
        selected = KeyRef("0", "custom:leaf-1", 1, "leaf-1", -1, 0)
        protected = KeyRef("0", "custom:leaf-99", 1, "dut", -1, 0)
        snapshots = AsyncMock(
            side_effect=(
                SimpleNamespace(self_node="dut", refs=(selected, protected)),
                SimpleNamespace(self_node="dut", refs=(protected,)),
            )
        )

        with (
            patch(f"{_MODULE}._scan_target", snapshots),
            patch(f"{_MODULE}._mark_snapshot", AsyncMock()),
            patch(f"{_MODULE}.asyncio.sleep", AsyncMock()),
        ):
            result = await run(
                _args(
                    key_regex=r"^custom:",
                    ttl_ms=1_000,
                    wait_sec=2,
                )
            )

        self.assertEqual(1, result)

    async def test_rejects_invalid_programmatic_timing(self) -> None:
        with self.assertRaisesRegex(ValueError, "wait_sec"):
            await expire_scale_keys_and_verify(
                ["dut.example"], ttl_ms=30_000, wait_sec=30
            )
