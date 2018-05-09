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
from builtins import range

import unittest
import random
import string

from thrift.protocol.TJSONProtocol import TJSONProtocolFactory

from openr.utils import serializer
from openr.Lsdb import ttypes as lsdb_types


class TestSerialization(unittest.TestCase):
    def test_reverse_equality(self):
        for _ in range(100):
            thrift_obj = lsdb_types.PrefixDatabase()
            random_string = ''.join(random.choice(string.digits) for _ in range(10))
            thrift_obj.thisNodeName = random_string
            raw_msg = serializer.serialize_thrift_object(thrift_obj)
            recovered_obj = serializer.deserialize_thrift_object(
                raw_msg, lsdb_types.PrefixDatabase)
            self.assertEqual(thrift_obj, recovered_obj)

        for _ in range(100):
            thrift_obj = lsdb_types.PrefixDatabase()
            random_string = ''.join(random.choice(string.digits) for _ in range(10))
            thrift_obj.thisNodeName = random_string
            raw_msg = serializer.serialize_thrift_object(
                thrift_obj, TJSONProtocolFactory)
            recovered_obj = serializer.deserialize_thrift_object(
                raw_msg, lsdb_types.PrefixDatabase, TJSONProtocolFactory)
            self.assertEqual(thrift_obj, recovered_obj)

    def test_thrifttype_sensitivity(self):
        thrift_obj = lsdb_types.PrefixDatabase()
        thrift_obj.thisNodeName = "some node"
        raw_msg = serializer.serialize_thrift_object(thrift_obj)
        recovered_obj = serializer.deserialize_thrift_object(
            raw_msg, lsdb_types.PrefixEntry)
        self.assertTrue(thrift_obj != recovered_obj)

    def test_exception_handling(self):
        thrift_obj = lsdb_types.PrefixDatabase()
        thrift_obj.thisNodeName = "some node"
        raw_msg = serializer.serialize_thrift_object(thrift_obj)
        # should raise exception due to inconsistency of protocol factor
        with self.assertRaises(Exception):
            serializer.deserialize_thrift_object(
                raw_msg, lsdb_types.PrefixDatabase, TJSONProtocolFactory)
