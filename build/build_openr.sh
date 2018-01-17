#!/bin/bash -e

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

set -ex

BUILD_DIR="$(readlink -f "$(dirname "$0")")"
export DESTDIR=""
mkdir -p "$BUILD_DIR/deps"
cd "$BUILD_DIR/deps"

find_github_hash() {
  if [[ $# -eq 1 ]]; then
    rev_file="github_hashes/$1-rev.txt"
    if [[ -f "$rev_file" ]]; then
      head -1 "$rev_file" | awk '{ print $3 }'
    fi
  fi
}

install_openr() {
  pushd .
  cd "$BUILD_DIR"
  cmake \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTS=ON \
    -DADD_ROOT_TESTS=ON \
    -DCMAKE_CXX_FLAGS="-Wno-unused-parameter" \
    ../openr/
  make
  make install
  chmod +x "/usr/local/sbin/run_openr.sh"
  cd "$BUILD_DIR/../openr/py"
  pip install cffi
  pip install future
  python setup.py build
  python setup.py install
  cd "$BUILD_DIR"
  #make test
  popd
}

#
# install other dependencies from source
#

install_openr

echo "OpenR built and installed successfully"
exit 0
