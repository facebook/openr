# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations

import unittest
from unittest.mock import MagicMock, patch

from openr.tests.scale.scripts import kvstore_cleanup


class ClientTransportTest(unittest.TestCase):
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

        for host in ("localhost", "LOCALHOST", "127.0.0.1", "0:0:0:0:0:0:0:1"):
            with self.subTest(host=host):
                self.assertIs(thrift_client, kvstore_cleanup._client(host, 2018))
                self.assertIsNone(get_client.call_args.kwargs["ssl_context"])

        self.assertEqual(4, get_ssl_context.call_count)
        self.assertEqual(4, get_client.call_count)

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
