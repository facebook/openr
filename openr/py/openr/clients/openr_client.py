#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import ssl

import bunch
from openr.py.openr.cli.utils.options import getDefaultOptions
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Platform.thrift_clients import FibService as FibServiceClient
from openr.thrift.Platform.thrift_types import FibClient
from thrift.python.client import ClientType, get_client, get_sync_client
from thrift.python.client.client_wrapper import Client, TAsyncClient, TSyncClient
from thrift.python.client.ssl import SSLContext, SSLVerifyOption
from thrift.python.exceptions import TransportError
from thrift.python.serializer import Protocol


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
