#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import random
import string
import unittest

from openr.thrift.Types import thrift_types as openr_types
from thrift.python.serializer import deserialize, serialize


class TestSerialization(unittest.TestCase):
    def test_reverse_equality(self) -> None:
        for _ in range(100):
            random_string = "".join(random.choice(string.digits) for _ in range(10))
            thrift_obj = openr_types.PrefixDatabase(thisNodeName=random_string)
            raw_msg = serialize(thrift_obj)
            recovered_obj = deserialize(openr_types.PrefixDatabase, raw_msg)
            self.assertEqual(thrift_obj, recovered_obj)

        for _ in range(100):
            random_string = "".join(random.choice(string.digits) for _ in range(10))
            thrift_obj = openr_types.PrefixDatabase(thisNodeName=random_string)
            raw_msg = serialize(thrift_obj)
            recovered_obj = deserialize(openr_types.PrefixDatabase, raw_msg)
            self.assertEqual(thrift_obj, recovered_obj)

    def test_thrifttype_sensitivity(self) -> None:
        thrift_obj = openr_types.PrefixDatabase(thisNodeName="some node")
        raw_msg = serialize(thrift_obj)
        recovered_obj = deserialize(openr_types.PrefixEntry, raw_msg)
        self.assertTrue(thrift_obj != recovered_obj)
