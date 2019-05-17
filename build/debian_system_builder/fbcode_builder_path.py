# Copyright (c) Facebook, Inc. and its affiliates.

from __future__ import absolute_import, division, print_function, unicode_literals
import os
import sys


# Add fbcode_builder directory to the path
sys.path.append(
    os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fbcode_builder"
    )
)
