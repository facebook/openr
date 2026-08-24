# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from openr.public_tld.build import breeze_package, gen


def create_thrift_source_tree(source_root):
    source_root = Path(source_root)
    openr_thrift_dir = source_root / "openr-thrift" / "openr" / "if"
    openr_thrift_dir.mkdir(parents=True)
    for thrift_file in (
        *gen.SUPPORTED_OPENR_THRIFT_FILES,
        *gen.EXCLUDED_OPENR_THRIFT_FILES,
    ):
        (openr_thrift_dir / thrift_file).touch()
    for include_dir in gen.THRIFT_INCLUDE_DIRS:
        (source_root / include_dir).mkdir(parents=True, exist_ok=True)
    for thrift_file in gen.REQUIRED_DEPENDENCY_THRIFT_FILES:
        dependency_file = source_root / thrift_file
        dependency_file.parent.mkdir(parents=True, exist_ok=True)
        dependency_file.touch()
    return openr_thrift_dir


def create_generated_package(generated_root):
    generated_root = Path(generated_root)
    for relative_path in breeze_package.EXPECTED_GENERATED_FILES:
        generated_file = generated_root / relative_path
        generated_file.parent.mkdir(parents=True, exist_ok=True)
        generated_file.touch()
    return generated_root


class CollectThriftFilesTest(unittest.TestCase):
    def test_collects_supported_openr_files_and_required_dependencies(self):
        with tempfile.TemporaryDirectory() as source_root:
            openr_thrift_dir = create_thrift_source_tree(source_root)

            thrift_files = gen.collect_thrift_files(source_root)

            self.assertEqual(
                thrift_files,
                [
                    *(
                        os.fspath(openr_thrift_dir / thrift_file)
                        for thrift_file in gen.SUPPORTED_OPENR_THRIFT_FILES
                    ),
                    *(
                        os.path.join(source_root, thrift_file)
                        for thrift_file in gen.REQUIRED_DEPENDENCY_THRIFT_FILES
                    ),
                ],
            )

    def test_rejects_manifest_drift(self):
        with tempfile.TemporaryDirectory() as source_root:
            openr_thrift_dir = Path(source_root) / "openr-thrift" / "openr" / "if"
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
            for include_dir in gen.THRIFT_INCLUDE_DIRS:
                (source_path / include_dir).mkdir(parents=True, exist_ok=True)

            with self.assertRaises(FileNotFoundError):
                gen.collect_thrift_files(source_root)

    def test_generates_only_modern_python_modules_in_one_output_tree(self):
        with tempfile.TemporaryDirectory() as source_root:
            thrift_files = gen.collect_thrift_files(
                os.fspath(create_thrift_source_tree(source_root).parents[2])
            )
            compiler = "/test/thrift1"
            stale_file = Path(source_root) / gen.GENERATED_THRIFT_DIR / "stale.pyx"
            stale_file.parent.mkdir(parents=True)
            stale_file.touch()

            with patch.object(gen, "check_call") as check_call:
                generated_root = gen.generate_thrift_files(source_root, compiler)

            output_root = os.path.join(source_root, gen.GENERATED_THRIFT_DIR)
            self.assertEqual(
                generated_root,
                os.path.join(output_root, "gen-python"),
            )
            self.assertEqual(len(thrift_files), check_call.call_count)
            self.assertFalse(stale_file.exists())
            for thrift_file, call in zip(thrift_files, check_call.call_args_list):
                command = call.args[0]
                self.assertEqual(command[:3], [compiler, "--gen", "mstch_python"])
                self.assertEqual(command[-3:], ["-o", output_root, thrift_file])
                self.assertNotIn("mstch_py3", command)
                self.assertNotIn("mstch_cpp2", command)

    def test_source_and_generated_module_manifests_match(self):
        source_modules = {
            Path(thrift_file).stem
            for thrift_file in (
                *gen.SUPPORTED_OPENR_THRIFT_FILES,
                *gen.REQUIRED_DEPENDENCY_THRIFT_FILES,
            )
        }
        generated_modules = {
            Path(module).name
            for module in (
                *breeze_package.NON_SERVICE_THRIFT_MODULES,
                *breeze_package.SERVICE_THRIFT_MODULES,
            )
        }

        self.assertSetEqual(source_modules, generated_modules)


class BreezePackageTest(unittest.TestCase):
    def test_stages_source_and_generated_namespace_packages(self):
        with tempfile.TemporaryDirectory() as temporary_root:
            temporary_root = Path(temporary_root)
            source_root = temporary_root / "source"
            generated_root = temporary_root / "generated"
            output_root = temporary_root / "package"
            source_file = source_root / "openr" / "py" / "openr" / "cli" / "breeze.py"
            source_file.parent.mkdir(parents=True)
            source_file.touch()
            setup_file = source_root / "openr" / "py" / "setup.py"
            setup_file.touch()
            create_generated_package(generated_root)
            stale_output = output_root / "stale.pyx"
            stale_output.parent.mkdir(parents=True)
            stale_output.touch()

            breeze_package.stage_breeze_package(
                source_root,
                generated_root,
                output_root,
            )

            self.assertTrue(
                (output_root / "openr" / "py" / "openr" / "cli" / "breeze.py").is_file()
            )
            self.assertTrue(
                (
                    output_root / "openr" / "thrift" / "Types" / "thrift_types.pyi"
                ).is_file()
            )
            self.assertTrue(
                (output_root / "fb303_core" / "thrift_clients.py").is_file()
            )
            self.assertTrue((output_root / "setup.py").is_file())
            self.assertFalse(stale_output.exists())
            staged_generated_files = {
                path.relative_to(output_root)
                for module in (
                    *breeze_package.NON_SERVICE_THRIFT_MODULES,
                    *breeze_package.SERVICE_THRIFT_MODULES,
                )
                for path in (output_root / module).iterdir()
                if path.is_file()
            }
            self.assertSetEqual(
                staged_generated_files,
                set(breeze_package.EXPECTED_GENERATED_FILES),
            )

    def test_rejects_incomplete_generated_package(self):
        with tempfile.TemporaryDirectory() as temporary_root:
            temporary_root = Path(temporary_root)

            with self.assertRaises(RuntimeError):
                breeze_package.stage_breeze_package(
                    temporary_root / "source",
                    temporary_root / "generated",
                    temporary_root / "package",
                )

    def test_rejects_unexpected_generated_artifact(self):
        with tempfile.TemporaryDirectory() as temporary_root:
            temporary_root = Path(temporary_root)
            source_root = temporary_root / "source"
            generated_root = create_generated_package(temporary_root / "generated")
            unexpected_file = (
                generated_root / "openr" / "thrift" / "Types" / "types.pyx"
            )
            unexpected_file.touch()
            source_file = source_root / "openr" / "py" / "openr" / "__init__.py"
            source_file.parent.mkdir(parents=True)
            source_file.touch()
            setup_file = source_root / "openr" / "py" / "setup.py"
            setup_file.touch()

            with self.assertRaisesRegex(RuntimeError, "unexpected .*types.pyx"):
                breeze_package.stage_breeze_package(
                    source_root,
                    generated_root,
                    temporary_root / "package",
                )


if __name__ == "__main__":
    unittest.main()
