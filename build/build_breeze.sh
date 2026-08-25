#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -euo pipefail

# shellcheck disable=SC1090,SC1091
source "$(dirname "$0")/common.sh"

BREEZE_VENV=/opt/openr-breeze
BREEZE_PACKAGE_DIR=/src/breeze-package
FBTHRIFT_BUILD_VENV=/opt/fbthrift-python-build
GENERATED_THRIFT_DIR=/src/generated-thrift/gen-python

# Stage the exact Thrift sources needed by the Breeze V1 package.
mkdir -p /src/fb303-thrift
cp -r /opt/facebook/fb303/include/thrift-files/fb303 /src/fb303-thrift/

FBTHRIFT_SOURCE_DIR=$(
  "$PYTHON3" "$GETDEPS" show-source-dir fbthrift
)
mkdir -p /src/fbthrift-thrift/thrift
cp -r "$FBTHRIFT_SOURCE_DIR/thrift/annotation" /src/fbthrift-thrift/thrift/

mkdir -p /src/openr-thrift/openr
cp -r /src/openr/if /src/openr-thrift/openr/

mkdir -p /src/neteng-thrift/configerator/structs/neteng/config
OPENR_NETENG_CONFIG_DIR=/src/configerator/structs/neteng/config
cp \
  "$OPENR_NETENG_CONFIG_DIR/routing_policy.thrift" \
  /src/neteng-thrift/configerator/structs/neteng/config/

cd /src
"$PYTHON3" /src/build/gen.py

"$PYTHON3" /src/build/breeze_package.py \
  --source-root /src \
  --generated-root "$GENERATED_THRIFT_DIR" \
  --output-root "$BREEZE_PACKAGE_DIR"

# Build the runtime with the same interpreter used to run Breeze.
"$PYTHON3" -m venv "$FBTHRIFT_BUILD_VENV"
FBTHRIFT_BUILD_PYTHON="$FBTHRIFT_BUILD_VENV/bin/python"
"$FBTHRIFT_BUILD_PYTHON" -m pip install \
  "cython==3.2.2" \
  auditwheel \
  setuptools \
  wheel
PATH="$FBTHRIFT_BUILD_VENV/bin:$PATH" \
  "$FBTHRIFT_BUILD_PYTHON" "$GETDEPS" \
  build \
  --allow-system-packages \
  --no-tests \
  --install-prefix "$INSTALL_PREFIX" \
  fbthrift-python

FBTHRIFT_PYTHON_INSTALL_DIR=$(
  "$FBTHRIFT_BUILD_PYTHON" "$GETDEPS" \
    show-inst-dir \
    --install-prefix "$INSTALL_PREFIX" \
    fbthrift-python
)

shopt -s nullglob
FBTHRIFT_PYTHON_WHEELS=("$FBTHRIFT_PYTHON_INSTALL_DIR"/share/thrift/wheels/*.whl)
if [ "${#FBTHRIFT_PYTHON_WHEELS[@]}" -ne 1 ]; then
  echo "Expected exactly one fbthrift-python wheel"
  exit 1
fi

"$PYTHON3" -m venv "$BREEZE_VENV"
BREEZE_PYTHON="$BREEZE_VENV/bin/python"
"$BREEZE_PYTHON" -m pip install \
  --no-deps \
  "${FBTHRIFT_PYTHON_WHEELS[0]}"

if ! "$BREEZE_PYTHON" -c "import folly.iobuf"; then
  echo "The fbthrift-python wheel does not contain its folly runtime"
  exit 1
fi

"$BREEZE_PYTHON" -m pip install "$BREEZE_PACKAGE_DIR"

BREEZE_PACKAGE_ROOT=$(
  "$BREEZE_PYTHON" -c "import site; print(site.getsitepackages()[0])"
)

cd /
"$BREEZE_PYTHON" /src/build/validate_breeze.py \
  --package-root "$BREEZE_PACKAGE_ROOT"

"$BREEZE_VENV/bin/breeze" --help >/dev/null
