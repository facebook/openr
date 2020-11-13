#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

# TODO: This all needs to go to cmake for thrift py3 support

import logging
import os
from pathlib import Path
from shutil import copytree
from subprocess import check_call
from sys import version_info


INSTALL_BASE_PATH = Path("/opt/facebook")
LOG = logging.getLogger(__name__)
# Set for Ubuntu Docker Container
# e.g. /usr/local/lib/python3.8/dist-packages/openr/
PY_PACKAGE_DIR = "/usr/local/lib/python{}.{}/dist-packages/".format(
    version_info.major, version_info.minor
)
CPP2_PATH = Path("/src/generated_cpp2")
PY3_HEADERS = INSTALL_BASE_PATH / "fbthrift/include/thrift/lib/py3"


def generate_thrift_files():
    """
    Get list of all thrift files (absolute path names) and then generate
    python definitions for all thrift files.
    """

    install_base = str(INSTALL_BASE_PATH)
    if "OPENR_INSTALL_BASE" in os.environ:
        install_base = os.environ["OPENR_INSTALL_BASE"]

    include_paths = (
        install_base,
        "{}/fb303/include/thrift-files/".format(install_base),
    )
    include_args = []
    for include_path in include_paths:
        include_args.extend(["-I", include_path])

    # /src/py/
    setup_py_path = Path(__file__).parent.absolute()
    # /src
    openr_root_path = setup_py_path.parent
    # /src/if/
    thrift_path = openr_root_path / "if"

    thrift_files = sorted(thrift_path.rglob("*.thrift"))
    LOG.info(
        "Going to build the following thrift files from {}: {}".format(
            str(thrift_path), thrift_files
        )
    )
    CPP2_PATH.mkdir(exist_ok=True)
    for thrift_file in thrift_files:
        for thrift_lang in ("py", "mstch_cpp2", "mstch_py3"):
            target_dir = PY_PACKAGE_DIR if 'py' in thrift_lang else str(CPP2_PATH)

            cmd = [
                "thrift1",
                "-r",
                "--gen",
                thrift_lang,
                "-I",
                str(openr_root_path),
                *include_args,
                "--out",
                target_dir,
                str(thrift_file),
            ]
            LOG.info(f"Running {' '.join(cmd)} ...")
            check_call(cmd)

            # TODO: Cython generated cpp2 with generated py3
            # If we're at py3 means we have CPP already generated
            if thrift_lang == 'mstch_py3':
                # exec cython with correct args to build ...
                LOG.debug("TODO: Run cython compile ...")
                pass

    # Copy fbthrift into thrift import dir
    fb_thrift_path = INSTALL_BASE_PATH / "fbthrift/lib/fb-py-libs/thrift_py/thrift"
    thrift_py_package_path = Path(PY_PACKAGE_DIR) / "thrift"
    copytree(str(fb_thrift_path), str(thrift_py_package_path), dirs_exist_ok=True)

    # Symlink fb303 python into PY_PACKAGE_DIR
    fb_fb303_path = (
        INSTALL_BASE_PATH / "fb303/lib/fb-py-libs/fb303_thrift_py/fb303_core"
    )
    fb303_py_pacakge_path = Path(PY_PACKAGE_DIR) / "fb303_core"
    if not fb303_py_pacakge_path.exists():
        fb303_py_pacakge_path.symlink_to(fb_fb303_path)

    # TODO: Build thrift.py + py3 fb303 libraries ... :|


if __name__ == "__main__":  # pragma: no cover
    logging.basicConfig(
        format="[%(asctime)s] %(levelname)s: %(message)s (%(filename)s:%(lineno)d)",
        level=logging.DEBUG,
    )
    LOG.info("Generating Open/R Thrift Libraries")
    generate_thrift_files()
