#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#


import ssl
import sys
from typing import Optional

import bunch
import zmq
from openr.cli.utils.options import getDefaultOptions
from openr.OpenrCtrl import OpenrCtrl
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.utils import consts, serializer, zmq_socket
from thrift.protocol import THeaderProtocol
from thrift.transport import THeaderTransport, TSocket, TSSLSocket


class OpenrCtrlClient(OpenrCtrl.Client):
    """
    Base class for for secure and plain-text clients. Do not use this
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
        ca_file: str,
        cert_file: str,
        key_file: str,
        acceptable_peer_name: str,
        port: int = consts.Consts.CTRL_PORT,
        timeout_ms: int = 5000,
    ) -> None:
        socket = TSSLSocket.TSSLSocket(
            host=host,
            port=port,
            cert_reqs=ssl.CERT_REQUIRED,
            ca_certs=ca_file,
            certfile=cert_file,
            keyfile=key_file,
            verify_name=acceptable_peer_name,
        )
        socket.setTimeout(timeout_ms)
        OpenrCtrlClient.__init__(self, host, THeaderTransport.THeaderTransport(socket))


def get_openr_ctrl_client(
    host: str, options: Optional[bunch.Bunch] = None
) -> OpenrCtrl.Client:
    """
    Utility function to get openr clients with default smart options. For
    options override please look at openr.cli.utils.options.OPTIONS
    """

    options = options if options else getDefaultOptions(host)
    if options.ssl:
        return OpenrCtrlSecureClient(
            host,
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


class OpenrClientDeprecated(object):
    """
    DEPRECATED - Use OpenrCtrlSecureClient or OpenrCtrlPlainTextClient
    """

    def __init__(self, module_type, zmq_endpoint, cli_opts):

        self.module_type = module_type
        self.zmq_endpoint = zmq_endpoint
        self.cli_opts = cli_opts
        self.thrift_client = None
        self.thrift_transport = None
        self.zmq_client = None

        if not self.cli_opts.prefer_zmq:
            if self.cli_opts.ssl:
                self.try_get_thrift_client(True)
            if self.thrift_client is None:
                self.try_get_thrift_client(False)

        if self.thrift_client is None:
            self.zmq_client = self.get_zmq_client()

    def __del__(self):
        self.cleanup_thrift()

    def check_for_module(self):
        assert self.thrift_client is not None
        try:
            return self.thrift_client.hasModule(self.module_type)
        except Exception:
            return False

    def get_thrift_client(self, use_ssl):
        socket = (
            TSSLSocket.TSSLSocket(
                host=self.cli_opts.host,
                port=self.cli_opts.openr_ctrl_port,
                # verify server
                cert_reqs=ssl.CERT_REQUIRED,
                ca_certs=self.cli_opts.ca_file,
                certfile=self.cli_opts.cert_file,
                keyfile=self.cli_opts.key_file,
                verify_name=self.cli_opts.acceptable_peer_name,
            )
            if use_ssl
            else TSocket.TSocket(
                host=self.cli_opts.host, port=self.cli_opts.openr_ctrl_port
            )
        )
        socket.setTimeout(self.cli_opts.timeout)
        transport = THeaderTransport.THeaderTransport(socket)
        transport.add_transform(THeaderTransport.TRANSFORM.ZSTD)
        protocol = THeaderProtocol.THeaderProtocol(transport)

        transport.open()
        self.thrift_transport = transport
        self.thrift_client = OpenrCtrl.Client(protocol)

    def cleanup_thrift(self):
        if self.thrift_transport is not None:
            self.thrift_transport.close()
            self.thrift_transport = None
        if self.thrift_client is not None:
            self.thrift_client = None

    def try_get_thrift_client(self, use_ssl):
        try:
            self.get_thrift_client(use_ssl)
            if not self.check_for_module():
                self.cleanup_thrift()
        except Exception:
            self.cleanup_thrift()

    def get_zmq_client(self, type=zmq.REQ):
        s = zmq_socket.ZmqSocket(
            self.cli_opts.zmq_ctx,
            type,
            self.cli_opts.timeout,
            self.cli_opts.proto_factory,
        )
        s.connect(self.zmq_endpoint)
        return s

    def send_and_recv_thrift_obj(self, thrift_obj_to_send, thrift_type_to_recv):
        req = serializer.serialize_thrift_object(
            thrift_obj_to_send, self.cli_opts.proto_factory
        )

        resp = None
        if self.thrift_client:
            try:
                resp = self.thrift_client.command(self.module_type, req)
            except Exception as e:
                print(
                    "Tried to connect via thrift but could not. Exception: "
                    "{}".format(e),
                    file=sys.stderr,
                )
                self.cleanup_thrift()
                raise e
        else:
            try:
                self.zmq_client.send(req)
                resp = self.zmq_client.recv()
            # TODO: remove after Link monitor socket is changed to ROUTER everywhere
            except Exception as e:
                if OpenrModuleType.LINK_MONITOR == self.module_type:
                    dealer_client = self.get_zmq_client(zmq.DEALER)
                    dealer_client.send(req)
                    resp = dealer_client.recv()
                else:
                    raise e
        if thrift_type_to_recv is str:
            return str(resp)

        return serializer.deserialize_thrift_object(
            resp, thrift_type_to_recv, self.cli_opts.proto_factory
        )
