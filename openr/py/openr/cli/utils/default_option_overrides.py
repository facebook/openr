#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from typing import Any

from bunch import Bunch


def getDefaultOption(options: Bunch, name: str) -> Any:
    return options[name]
