#!/bin/bash

CFG_BASE="/config"
DEFAULT_CFG="/etc/openr.conf"
FB_BASE="/opt/facebook"
OPENR_CFG="${CFG_BASE}/openr.conf"

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

mkdir -p "$CFG_BASE"
if [ ! -s "$OPENR_CFG" ]
then
  echo "Copying default Open/R config - Please check settings!"
  cp -vp "$DEFAULT_CFG" "$CFG_BASE"
fi

echo "[$(date)] Attempting to start Open/R using "$OPENR_CFG" $@"
openr_bin -v 2 --config "$OPENR_CFG" $@
