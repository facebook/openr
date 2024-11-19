#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import ssl
from typing import Optional, Type

import bunch
from openr.OpenrCtrl import OpenrCtrl
from openr.py.openr.cli.utils.options import getDefaultOptions
from openr.py.openr.utils import consts
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Platform.thrift_clients import FibService as FibServiceClient
from openr.thrift.Platform.thrift_types import FibClient
from thrift.protocol import THeaderProtocol
from thrift.python.client import ClientType, get_client, get_sync_client
from thrift.python.client.client_wrapper import Client, TAsyncClient, TSyncClient
from thrift.python.client.ssl import SSLContext, SSLVerifyOption
from thrift.python.exceptions import TransportError
from thrift.python.serializer import Protocol
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


def get_ssl_context(
    options: bunch.Bunch,
) -> SSLContext | None:
    # The options are the local settings to connect to the host

    ssl_context: SSLContext | None = None
    # Create ssl context if specified
    if options.ssl:
        # Translate ssl verification option
        ssl_verify_opt = SSLVerifyOption.NO_VERIFY
        if options.cert_reqs == ssl.CERT_OPTIONAL:
            ssl_verify_opt = SSLVerifyOption.VERIFY_REQ_CLIENT_CERT
        if options.cert_reqs == ssl.CERT_REQUIRED:
            ssl_verify_opt = SSLVerifyOption.VERIFY

        # Create ssl context
        ssl_context = SSLContext()
        ssl_context.set_verify_option(ssl_verify_opt)
        ssl_context.load_cert_chain(
            certfile=options.cert_file, keyfile=options.key_file
        )
        ssl_context.load_verify_locations(cafile=options.ca_file)
    return ssl_context


def get_fib_agent_client(
    host: str,
    port: int,
    timeout_ms: int,
    client_id: int = FibClient.OPENR,
    client_class: type[
        Client[TAsyncClient, TSyncClient]
    ] = FibServiceClient,  # Allow service client overwrite
) -> TSyncClient:  # return client_class.Sync
    """
    Get thrift-python sync client for talking to Fib thrift service

    :param host: thrift server name or ip
    :param port: thrift server port

    :returns: The (sync) thrift-python client

    NOTE: We get a sync client here as we currently use sync clients in FibAgentCmd.
    When we migrate to async client, we just need to change the function
    `get_sync_client` below to `get_client`, which provides the async client.
    """

    ssl_context: SSLContext | None = get_ssl_context(getDefaultOptions(host))
    try:
        client = get_sync_client(
            client_class,
            host=host,
            port=port,
            timeout=float(timeout_ms) / 1000.0,  # NOTE: Timeout expected is in seconds
            client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
            protocol=Protocol.BINARY,
            ssl_context=ssl_context,
            ssl_timeout=float(timeout_ms)
            / 1000.0,  # NOTE: Timeout expected is in seconds
        )
    except TransportError as e:
        if ssl_context is not None:
            # cannot establish tls, fallback to plain text
            client = get_sync_client(
                client_class,
                host=host,
                port=port,
                timeout=float(timeout_ms)
                / 1000.0,  # NOTE: Timeout expected is in seconds
                client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
                protocol=Protocol.BINARY,
                ssl_context=None,
                ssl_timeout=float(timeout_ms)
                / 1000.0,  # NOTE: Timeout expected is in seconds
            )
        else:
            raise e

    # Assign so that we can refer to later on
    # Pyre does not allow us to assign a non-existing attribute to FibServiceClient
    # Therefore we need to explicitly ignore the error
    client.client_id = client_id
    return client


def get_openr_ctrl_cpp_sync_client(
    host: str,
    options: bunch.Bunch | None = None,
    client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
) -> OpenrCtrlCppClient.Sync:
    """
    Utility function to get thrift-python SYNC OpenrClient.
    Migrate all of our client use-case to thrift-python as python2 support is deprecated.
    https://fburl.com/ef0eq78f
    """

    options = options if options else getDefaultOptions(host)

    # Create and return client
    ssl_context: SSLContext | None = get_ssl_context(options)
    try:
        client = get_sync_client(
            OpenrCtrlCppClient,
            host=host,
            port=options.openr_ctrl_port,
            timeout=(options.timeout / 1000),  # NOTE: Timeout expected is in seconds
            client_type=client_type,
            ssl_context=ssl_context,
            ssl_timeout=(
                options.timeout / 1000
            ),  # NOTE: Timeout expected is in seconds
        )
    except TransportError as e:
        if ssl_context is not None:
            # cannot establish tls, fallback to plain text
            client = get_sync_client(
                OpenrCtrlCppClient,
                host=host,
                port=options.openr_ctrl_port,
                timeout=(
                    options.timeout / 1000
                ),  # NOTE: Timeout expected is in seconds
                client_type=client_type,
                ssl_context=None,
                ssl_timeout=(
                    options.timeout / 1000
                ),  # NOTE: Timeout expected is in seconds
            )
        else:
            raise e

    return client


def get_openr_ctrl_cpp_client(
    host: str,
    options: bunch.Bunch | None = None,
    client_type=ClientType.THRIFT_ROCKET_CLIENT_TYPE,
) -> OpenrCtrlCppClient.Async:
    """
    Utility function to get thrift-python async OpenrClient. We must eventually move all of our
    client use-case to thrift-python as python2 support is deprecated.
    https://fburl.com/ef0eq78f

    Major Usecase for: py3 supports streaming
    """

    options = options if options else getDefaultOptions(host)

    # Create and return client
    ssl_context: SSLContext | None = get_ssl_context(options)
    try:
        client = get_client(
            OpenrCtrlCppClient,
            host=host,
            port=options.openr_ctrl_port,
            timeout=(options.timeout / 1000),  # NOTE: Timeout expected is in seconds
            client_type=client_type,
            ssl_context=ssl_context,
            ssl_timeout=(
                options.timeout / 1000
            ),  # NOTE: Timeout expected is in seconds
        )
    except TransportError as e:
        if ssl_context is not None:
            # cannot establish tls, fallback to plain text
            client = get_client(
                OpenrCtrlCppClient,
                host=host,
                port=options.openr_ctrl_port,
                timeout=(
                    options.timeout / 1000
                ),  # NOTE: Timeout expected is in seconds
                client_type=client_type,
                ssl_context=None,
                ssl_timeout=(
                    options.timeout / 1000
                ),  # NOTE: Timeout expected is in seconds
            )
        else:
            raise e

    return client
