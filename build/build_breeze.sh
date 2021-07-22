#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# shellcheck disable=SC1090
source "$(dirname "$0")/common.sh"

alias thrift1=/opt/facebook/fbthrift/bin/thrift1

# Step 1: Stage compilation

# folly Cython
mkdir ./folly
cp -r \
/tmp/fbcode_builder_getdeps-ZsrcZbuildZfbcode_builder-root/repos/github.com-facebook-folly.git/folly/python/* ./folly

# fb303 thrift
mkdir -p ./fb303-thrift
cp -r  /opt/facebook/fb303/include/thrift-files/fb303 ./fb303-thrift/

# fbthrift Cython and thrift
cp -r  /opt/facebook/fbthrift/include/thrift/lib/ ./thrift
touch ./thrift/py3/__init__.py
touch ./thrift/__init__.py

mkdir /src/fbthrift-thrift
cp -r \
/tmp/fbcode_builder_getdeps-ZsrcZbuildZfbcode_builder-root/repos/github.com-facebook-fbthrift.git/thrift/lib/thrift/* \
/src/fbthrift-thrift

mkdir -p /src/thrift/py3
chown -R root /tmp/fbcode_builder_getdeps-ZsrcZbuildZfbcode_builder-root/repos/github.com-facebook-fbthrift.git/thrift/lib/py3/
cp -r \
/tmp/fbcode_builder_getdeps-ZsrcZbuildZfbcode_builder-root/repos/github.com-facebook-fbthrift.git/thrift/lib/py3/* \
/src/thrift/py3
touch /src/thrift/__init__.py

# Open/R thrift
mkdir -p ./openr-thrift/openr
cp -r /src/openr/if/ ./openr-thrift/openr/

# Neteng thrift
mkdir -p neteng-thrift/configerator/structs/neteng/config/
cp -r \
/tmp/fbcode_builder_getdeps-ZsrcZbuildZfbcode_builder-root/repos/github.com-facebook-openr.git/configerator/structs/neteng/config/routing_policy.thrift \
neteng-thrift/configerator/structs/neteng/config/

# HACK TO FIX CYTHON .pxd
cp /cython/Cython/Includes/libcpp/utility.pxd /usr/lib/python3/dist-packages/Cython/Includes/libcpp/utility.pxd

# Step 2. Generate mstch_cpp2 and mstch_py3 bindings

python3 /src/build/gen.py

# HACK TO FIX fbthrift-py/gen-cpp2/
echo " " > /src/fbthrift-thrift/gen-cpp2/metadata_metadata.h
echo " " > /src/fbthrift-thrift/gen-cpp2/metadata_types.h
echo " " > /src/fbthrift-thrift/gen-cpp2/metadata_types_custom_protocol.h

# Step 3. Generate clients.cpp

python3 /src/build/cython_compile.py

# Step 4. Compile .so

# shellcheck disable=SC2097,SC2098
env \
CC="/usr/bin/gcc-10" \
CXX="/usr/bin/g++-10" \
CFLAGS="-I. -Iopenr-thrift -Ifb303-thrift -Ineteng-thrift -std=c++20 -fcoroutines " \
CFLAGS="$CFLAGS -w -D_CPPLIB_VER=20" \
CXXFLAGS="$CFLAGS" \
python3 openr/py/setup.py build -j10
