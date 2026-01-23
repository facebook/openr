#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import bunch
from openr.OpenrCtrl import OpenrCtrl
from openr.py.openr.cli.utils.options import getDefaultOptions
from openr.py.openr.utils import consts
from thrift.protocol import THeaderProtocol
from thrift.transport import THeaderTransport, TSocket, TSSLSocket


class OpenrCtrlClient(OpenrCtrl.Client):
    """
    Base class for secure and plain-text clients. Do not use this
    client directly. Instead use one of `OpenrCtrlPlainTextClient` or
    `OpenrCtrlSecureClient`
    """

    def __init__(self, host: str, transport: THeaderTransport.THeaderTransport) -> None:
        self.host = host  # Just for accessibility
        self._transport = transport
        self._transport.add_transform(THeaderTransport.TRANSFORM.ZSTD)
        OpenrCtrl.Client.__init__(
            self, THeaderProtocol.THeaderProtocol(self._transport)
        )

    def __enter__(self):
        self._transport.open()
        return self

    def __exit__(self, type, value, traceback):
        self._transport.close()
        self._transport = None


class OpenrCtrlPlainTextClient(OpenrCtrlClient):
    """
    PlainText Thrift client for Open/R

    Prefer to use this for onbox communications or when secure thrift
    infrastructure is not setup/available
    """

    def __init__(
        self, host: str, port: int = consts.Consts.CTRL_PORT, timeout_ms: int = 5000
    ) -> None:
        socket = TSocket.TSocket(host=host, port=port)
        socket.setTimeout(timeout_ms)
        OpenrCtrlClient.__init__(self, host, THeaderTransport.THeaderTransport(socket))


class OpenrCtrlSecureClient(OpenrCtrlClient):
    """
    Secure Thrift client for Open/R

    Prefer to use this for remote communications
    """

    def __init__(
        self,
        host: str,
        cert_reqs: int,
        ca_file: str,
        cert_file: str,
        key_file: str,
        acceptable_peer_name: str,
        port: int = consts.Consts.CTRL_PORT,
        timeout_ms: int = 5000,
    ) -> None:
        verify_name = acceptable_peer_name if acceptable_peer_name != "" else None
        socket = TSSLSocket.TSSLSocket(
            host=host,
            port=port,
            cert_reqs=cert_reqs,
            ca_certs=ca_file,
            certfile=cert_file,
            keyfile=key_file,
            verify_name=verify_name,
        )
        socket.setTimeout(timeout_ms)
        OpenrCtrlClient.__init__(self, host, THeaderTransport.THeaderTransport(socket))


# to be deprecated
def get_openr_ctrl_client_py(
    host: str, options: bunch.Bunch | None = None
) -> OpenrCtrl.Client:
    """
    Utility function to get openr clients with default smart options. For
    options override please look at openr.cli.utils.options.OPTIONS
    """

    options = options if options else getDefaultOptions(host)
    if options.ssl:
        return OpenrCtrlSecureClient(
            host,
            options.cert_reqs,
            options.ca_file,
            options.cert_file,
            options.key_file,
            options.acceptable_peer_name,
            options.openr_ctrl_port,
            options.timeout,
        )
    else:
        return OpenrCtrlPlainTextClient(
            options.host, options.openr_ctrl_port, options.timeout
        )
