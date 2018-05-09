#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import
from __future__ import print_function
from __future__ import unicode_literals
from __future__ import division
from builtins import str
from builtins import range
from builtins import object

import zmq

from openr.utils import serializer, consts


# enumeration of socket status
class SocketStatus(object):
    connected, binded, idle = list(range(3))


class Socket(object):

    def __init__(self, zmq_ctx, socket_type, timeout=consts.Consts.TIMEOUT_MS,
                 proto_factory=consts.Consts.PROTO_FACTORY):
        ''' Initialize socket '''

        self.zmq_ctx = zmq_ctx
        self.socket_type = socket_type
        self.timeout = timeout
        self.proto_factory = proto_factory

        self._status = SocketStatus.idle
        self._socket = self.zmq_ctx.socket(self.socket_type)
        self._socket.set(zmq.IPV6, 1)
        self._socket.set(zmq.LINGER, 0)

    def _config_socket(self, socket_url, method):

        options = {'connect': [self._socket.connect, SocketStatus.connected],
                   'bind': [self._socket.bind, SocketStatus.binded]}

        try:
            options[method][0](socket_url)
            self._status = options[method][1]
            return self._socket
        except Exception as ex:
            raise Exception("Failed {}ing: {}".format(method, ex))

    def connect(self, socket_url):
        ''' Connect socket '''

        if self._status == SocketStatus.binded:
            raise Exception("Socket is not allowed to connect after binding")

        return self._config_socket(socket_url, 'connect')

    def bind(self, socket_url):
        ''' Bind socket '''

        if self._status == SocketStatus.connected:
            raise Exception("Socket is not allowed to bind after connecting")

        return self._config_socket(socket_url, 'bind')

    def recv(self):
        ''' Receives data on socket with a timeout to avoid infinite wait. '''

        if self._socket is None:
            raise Exception('Socket is not set')

        if self.timeout != -1:
            self._socket.poll(self.timeout, zmq.POLLIN)
            return self._socket.recv(zmq.NOBLOCK)
        else:
            return self._socket.recv()

    def recv_thrift_obj(self, thrift_type):
        ''' Receive on socket with or without a timeout and deserialize to
            thrift_type '''

        raw_msg = self.recv()
        return serializer.deserialize_thrift_object(raw_msg, thrift_type,
                                                    self.proto_factory)

    def send(self, data):
        ''' send binary data on socket '''

        try:
            self._socket.send(str(data))
        except Exception as ex:
            raise Exception("Failed sending message: {}".format(ex))

    def send_thrift_obj(self, thrift_obj):
        ''' Serialize thrift object and send to socket '''

        raw_msg = serializer.serialize_thrift_object(thrift_obj, self.proto_factory)
        try:
            self._socket.send(raw_msg)
        except Exception as ex:
            raise Exception("Failed sending message: {}".format(ex))

    def set_sock_opt(self, opt, value):
        ''' set default socket options for socket '''

        self._socket.setsockopt(opt, value)

    def get(self):
        ''' get raw socket '''

        return self._socket

    def close(self):
        ''' Close socket '''

        self._socket.close()
