# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
from subprocess import Popen


def wait_for_processes(procs):
    failures = 0
    for proc in procs:
        proc.wait()
        if proc.returncode != 0:
            failures += 1
    return failures


def compile_cython_modules():
    procs = []
    for root, _dirs, files in os.walk("openr-thrift"):
        for file in files:
            if file.endswith(".pyx"):
                thrift_file = os.path.join(root, file)
                cmd = [
                    "cython3",
                    "--fast-fail",
                    "-3",
                    "--cplus",
                    thrift_file,
                    "-o",
                    root,
                    "-I.",
                    "-I/src",
                    "-I/usr/lib/python3/dist-packages/Cython/Includes",
                    "-I/src/fbthrift-thrift/gen-py3",
                    "-I/src/fb303-thrift/fb303/thrift/gen-py3",
                    "-I/src/neteng-thrift/configerator/structs/neteng/config/gen-py3",
                ]
                print(f"Generating cython module {file}")
                procs.append(Popen(cmd))

    print("Waiting for cython generation to finish...")
    failures = wait_for_processes(procs)
    print(f"{len(procs) - failures}/{len(procs)} succeeded")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    compile_cython_modules()
