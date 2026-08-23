# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from openr.public_tld.build import cython_compile, gen


class FakeProcess:
    def __init__(self, returncode):
        self.returncode = returncode

    def wait(self):
        pass


class CollectThriftFilesTest(unittest.TestCase):
    def test_collects_supported_openr_files_and_staged_dependencies(self):
        with tempfile.TemporaryDirectory() as source_root:
            source_path = Path(source_root)
            openr_thrift_dir = source_path / "openr-thrift" / "openr" / "if"
            openr_thrift_dir.mkdir(parents=True)
            for thrift_file in gen.SUPPORTED_OPENR_THRIFT_FILES:
                (openr_thrift_dir / thrift_file).touch()
            unsupported_v2 = openr_thrift_dir / gen.EXCLUDED_OPENR_THRIFT_FILES[0]
            unsupported_v2.touch()

            expected_dependency_files = []
            for thrift_dir in gen.DEPENDENCY_THRIFT_DIRS:
                if thrift_dir == "neteng-thrift":
                    continue
                dependency_file = source_path / thrift_dir / "dependency.thrift"
                dependency_file.parent.mkdir(parents=True)
                dependency_file.touch()
                expected_dependency_files.append(os.fspath(dependency_file))
            required_dependency_files = []
            for thrift_file in gen.REQUIRED_DEPENDENCY_THRIFT_FILES:
                dependency_file = source_path / thrift_file
                dependency_file.parent.mkdir(parents=True, exist_ok=True)
                dependency_file.touch()
                required_dependency_files.append(os.fspath(dependency_file))
            expected_dependency_files[1:1] = required_dependency_files

            thrift_files = gen.collect_thrift_files(source_root)

            self.assertNotIn(os.fspath(unsupported_v2), thrift_files)
            self.assertEqual(
                thrift_files,
                [
                    *(
                        os.fspath(openr_thrift_dir / thrift_file)
                        for thrift_file in gen.SUPPORTED_OPENR_THRIFT_FILES
                    ),
                    *expected_dependency_files,
                ],
            )

    def test_rejects_manifest_drift(self):
        with tempfile.TemporaryDirectory() as source_root:
            source_path = Path(source_root)
            openr_thrift_dir = source_path / "openr-thrift" / "openr" / "if"
            openr_thrift_dir.mkdir(parents=True)

            with self.assertRaises(RuntimeError):
                gen.collect_thrift_files(source_root)

    def test_rejects_missing_required_dependency(self):
        with tempfile.TemporaryDirectory() as source_root:
            source_path = Path(source_root)
            openr_thrift_dir = source_path / "openr-thrift" / "openr" / "if"
            openr_thrift_dir.mkdir(parents=True)
            for thrift_file in (
                *gen.SUPPORTED_OPENR_THRIFT_FILES,
                *gen.EXCLUDED_OPENR_THRIFT_FILES,
            ):
                (openr_thrift_dir / thrift_file).touch()
            for thrift_dir in gen.DEPENDENCY_THRIFT_DIRS:
                (source_path / thrift_dir).mkdir(parents=True)

            with self.assertRaises(FileNotFoundError):
                gen.collect_thrift_files(source_root)


class WaitForProcessesTest(unittest.TestCase):
    def test_counts_failed_processes(self):
        processes = [FakeProcess(0), FakeProcess(1), FakeProcess(2)]

        self.assertEqual(2, cython_compile.wait_for_processes(processes))

    def test_compile_exits_when_cython_fails(self):
        with (
            patch.object(
                cython_compile.os,
                "walk",
                return_value=[("openr-thrift", [], ["broken.pyx"])],
            ),
            patch.object(cython_compile, "Popen", return_value=FakeProcess(1)),
        ):
            with self.assertRaises(SystemExit):
                cython_compile.compile_cython_modules()


if __name__ == "__main__":
    unittest.main()
