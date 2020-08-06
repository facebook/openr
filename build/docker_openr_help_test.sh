#!/bin/bash

FB_BASE="/opt/facebook"

LIBZMQ="${FB_BASE}/libzmq-WghZ2BfkSXrO-iCLPPsrNxYETqFoseeoIpU2B0rm7-s/lib/"
LIBSODIUM="${FB_BASE}/libsodium-EEVS7zAI4ySTAo4CetZWCIB2xbiPtFMxRa6icbw0Tqs/lib/"
GLOG="${FB_BASE}/glog-wEZy5J_sLgqVSmKEalImow54X6-wimELnMs7_Sbq-Qw/lib/"
GFLAGS="${FB_BASE}/gflags-f6oudili_zRvs7dpAJ1CVNIAKwWD3H63YKHyhKHXqL8/lib/"
SNAPPY="${FB_BASE}/snappy-ZcP2UwbImF5TbOuXCwLmAlVK7EBc8aIB6OIgQuCTc24/lib/"


export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${LIBZMQ}:${LIBSODIUM}:${GLOG}:${GFLAGS}:${SNAPPY}"
export PATH="${PATH}:${FB_BASE}/openr/sbin"

openr_bin --help
