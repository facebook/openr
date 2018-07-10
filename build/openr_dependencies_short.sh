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

install_zstd() {
  pushd .
  if [[ ! -e "zstd" ]]; then
    git clone https://github.com/facebook/zstd
  fi
  cd zstd
  make
  make install
  ldconfig
  popd
}

install_mstch() {
  pushd .
  if [[ ! -e "mstch" ]]; then
    git clone https://github.com/no1msd/mstch
  fi
  cd mstch
  cmake -DBUILD_SHARED_LIBS=ON -DCMAKE_CXX_FLAGS="-fPIC" .
  make
  make install
  ldconfig
  popd
}

install_wangle() {
  pushd .
  if [[ ! -e "wangle" ]]; then
    git clone https://github.com/facebook/wangle
  fi
  rev=$(find_github_hash facebook/wangle)
  cd wangle/wangle
  if [[ ! -z "$rev" ]]; then
    git fetch origin
    git checkout "$rev"
  fi
  cmake \
    -DFOLLY_INCLUDE_DIR=$DESTDIR/usr/local/include \
    -DFOLLY_LIBRARY=$DESTDIR/usr/local/lib \
    -DBUILD_TESTS=OFF .
  make
  make install
  ldconfig
  popd
}

install_libzmq() {
  pushd .
  if [[ ! -e "libzmq" ]]; then
    git clone https://github.com/zeromq/libzmq
  fi
  cd libzmq
  git fetch origin
  git checkout v4.2.2
  ./autogen.sh
  ./configure
  make
  make install
  ldconfig
  popd
}

install_libsodium() {
  pushd .
  if [[ ! -e "libsodium" ]]; then
    git clone https://github.com/jedisct1/libsodium --branch stable
  fi
  cd libsodium
  ./configure
  make
  make install
  ldconfig
  popd
}

install_folly() {
  pushd .
  if [[ ! -e "folly" ]]; then
    git clone https://github.com/facebook/folly
  fi
  rev=$(find_github_hash facebook/folly)
  cd folly/folly
  if [[ ! -z "$rev" ]]; then
    git fetch origin
    git checkout "$rev"
  fi
  autoreconf -ivf
  ./configure LIBS="-lpthread"
  make
  make install
  ldconfig
  popd
}

install_fbthrift() {
  pushd .
  if [[ ! -e "fbthrift" ]]; then
    git clone https://github.com/facebook/fbthrift
  fi
  rev=$(find_github_hash facebook/fbthrift)
  cd fbthrift/build
  if [[ ! -z "$rev" ]]; then
    git fetch origin
    git checkout "$rev"
  fi
  cmake -DBUILD_SHARED_LIBS=ON ..
  make
  make install
  ldconfig
  cd ../thrift/lib/py
  python setup.py install
  popd
}

install_fbzmq() {
  pushd .
  if [[ ! -e "fbzmq" ]]; then
    git clone https://github.com/facebook/fbzmq.git
  fi
  rev=$(find_github_hash facebook/fbzmq)
  cd fbzmq/build
  if [[ ! -z "$rev" ]]; then
    git fetch origin
    git checkout "$rev"
  fi
  cmake \
    -DCMAKE_CXX_FLAGS="-Wno-sign-compare -Wno-unused-parameter" \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_FIND_LIBRARY_SUFFIXES=".a" \
    -DBUILD_TESTS=OFF \
    ../fbzmq
  make
  make install
  ldconfig
  cd ../fbzmq/py
  python setup.py install
  popd
}

install_glog() {
  pushd .
  if [[ ! -e "glog" ]]; then
    git clone https://github.com/google/glog
  fi
  cd glog
  git fetch origin
  git checkout v0.3.5
  set -eu && autoreconf -i
  ./configure
  make
  make install
  ldconfig
  popd
}

install_gflags() {
  pushd .
  if [[ ! -e "gflags" ]]; then
    git clone https://github.com/gflags/gflags
  fi
  cd gflags
  git fetch origin
  git checkout v2.2.0
  if [[ ! -e "mybuild" ]]; then
    mkdir mybuild
  fi
  cd mybuild
  cmake -DBUILD_SHARED_LIBS=ON ..
  make
  make install
  ldconfig
  popd
}

install_gtest() {
  pushd .
  if [[ ! -e "googletest" ]]; then
    git clone https://github.com/google/googletest
  fi
  cd googletest
  git fetch origin
  git checkout release-1.8.0
  cd googletest
  cmake .
  make
  make install
  ldconfig
  cd ../googlemock
  cmake .
  make
  make install
  ldconfig
  popd
}

install_libnl() {
  pushd .
  if [[ ! -e "libnl" ]]; then
    git clone https://github.com/thom311/libnl
    cd libnl
    git fetch origin
    git checkout libnl3_2_25
    git apply ../../fix-route-obj-attr-list.patch
    cd ..
  fi
  cd libnl
  ./autogen.sh
  ./configure
  make
  make install
  ldconfig
  popd
}

#
# Install required tools and libraries via package managers
#

apt-get install -y libdouble-conversion-dev \
  libssl-dev \
  cmake \
  make \
  zip \
  git \
  autoconf \
  autoconf-archive \
  automake \
  libtool \
  g++ \
  libboost-all-dev \
  libevent-dev \
  flex \
  bison \
  liblz4-dev \
  liblzma-dev \
  scons \
  libkrb5-dev \
  libsnappy-dev \
  libsasl2-dev \
  libnuma-dev \
  pkg-config \
  zlib1g-dev \
  binutils-dev \
  libjemalloc-dev \
  libiberty-dev \
  python-setuptools \
  python3-setuptools \
  python-pip

#
# install other dependencies from source
#

install_glog
install_gflags
install_folly


echo "OpenR short Dependencies built and installed successfully"
exit 0
