#!/bin/bash

FB_BASE="/opt/facebook"
OPENR_CFG="/config/openr.cfg"

LIBZMQ="${FB_BASE}/libzmq-*/lib/"
LIBSODIUM="${FB_BASE}/libsodium-*/lib/"
GLOG="${FB_BASE}/glog-*/lib/"
GFLAGS="${FB_BASE}/gflags-*/lib/"
SNAPPY="${FB_BASE}/snappy-*/lib/"


# Hack to fix environment for openr linking
for dep in $LIBZMQ $LIBSODIUM $GLOG $GFLAGS $SNAPPY
do
  export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$(eval echo ${dep})"
done
export PATH="${PATH}:${FB_BASE}/openr/sbin"

CONFIG_OPT=""
if [ -s "$OPENR_CFG" ]
then
  CONFIG_OPT="--config $OPENR_CFG"
fi

openr_bin $CONFIG_OPT $@
