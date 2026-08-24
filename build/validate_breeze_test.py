# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import tempfile
import unittest
from pathlib import Path

from openr.public_tld.build.validate_breeze import validate_package_contents


class ValidatePackageContentsTest(unittest.TestCase):
    def test_accepts_pure_python_openr_package(self):
        with tempfile.TemporaryDirectory() as package_root:
            package_file = Path(package_root, "openr", "thrift_types.py")
            package_file.parent.mkdir(parents=True)
            package_file.touch()

            self.assertIsNone(validate_package_contents(package_root))

    def test_rejects_native_openr_artifacts(self):
        for suffix in (".cpp", ".pyx", ".so"):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as root:
                artifact = Path(root, "openr", f"generated{suffix}")
                artifact.parent.mkdir(parents=True)
                artifact.touch()

                with self.assertRaisesRegex(RuntimeError, artifact.name):
                    validate_package_contents(root)
