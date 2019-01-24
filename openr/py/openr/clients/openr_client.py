#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import, division, print_function, unicode_literals

import ssl
import sys

import zmq
from openr.OpenrCtrl import OpenrCtrl
from openr.OpenrCtrl.ttypes import OpenrModuleType
from openr.utils import serializer, zmq_socket
from thrift.protocol import THeaderProtocol
from thrift.transport import THeaderTransport, TSocket, TSSLSocket


class OpenrClient(object):
    def __init__(self, module_type, zmq_endpoint, cli_opts):

        self.module_type = module_type
        self.zmq_endpoint = zmq_endpoint
        self.cli_opts = cli_opts
        self.thrift_has_module = (
            not self.cli_opts.prefer_zmq and self.check_for_module()
        )

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
        protocol = THeaderProtocol.THeaderProtocol(transport)

        transport.open()
        return OpenrCtrl.Client(protocol)

    def check_for_module(self):
        if self.cli_opts.ssl:
            try:
                return self.get_thrift_client(True).hasModule(self.module_type)
            except Exception:
                pass
        try:
            return self.get_thrift_client(False).hasModule(self.module_type)
        except Exception:
            return False

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
        if self.thrift_has_module:
            # we should be able to connect to this module via the thriftCtrl
            # server. if we think SSL is enabled on the server, lets try that
            # first
            if self.cli_opts.ssl:
                try:
                    resp = self.get_thrift_client(True).command(self.module_type, req)
                except Exception as e:
                    print(
                        "Tried to connect via secure thrift but could not. "
                        "Exception: {}.".format(e),
                        file=sys.stderr,
                    )
            if resp is None:
                try:
                    resp = self.get_thrift_client(False).command(self.module_type, req)
                except Exception as e:
                    print(
                        "Tried to connect via plaintext thrift but could not. "
                        "Exception: {}.".format(e),
                        file=sys.stderr,
                    )

        if resp is None:
            try:
                zmq_client = self.get_zmq_client()
                zmq_client.send(req)
                resp = zmq_client.recv()
            # TODO: remove after Link monitor socket is changed to ROUTER everywhere
            except Exception as e:
                if OpenrModuleType.LINK_MONITOR == self.module_type:
                    zmq_client = self.get_zmq_client(zmq.DEALER)
                    zmq_client.send(req)
                    resp = zmq_client.recv()
                else:
                    raise e

        if thrift_type_to_recv is str:
            return str(resp)

        return serializer.deserialize_thrift_object(
            resp, thrift_type_to_recv, self.cli_opts.proto_factory
        )
