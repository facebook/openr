#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

#
# Parse arguments
#
alter="false"
case $1 in
  check)
    alter="false"
    ;;
  install)
    alter="true"
    ;;
  *)
    echo "Unknown argument: $1"
    exit 1
esac


#
# Openr needs to use default namespace if it is there
#
default=""
if ip netns exec default
then
  default="ip netns exec default"
fi

#
# Open up Openr TCP port range for v4 and v6.
#

if $default ip6tables -nL INPUT --line-numbers | grep -P "openr_v6_tcp_ports"
then
  echo "ip6tables allow for openr TCP ports is already present";
else
  if [ $alter == "false" ]; then
    exit 1
  fi
  echo "adding ip6tables allow for openr TCP ports";
  $default ip6tables -A INPUT -p tcp --match multiport --dports 60000:60100 -j ACCEPT -m comment --comment "openr_v6_tcp_ports"
fi

if $default iptables -nL INPUT --line-numbers | grep -P "openr_v4_tcp_ports"
then
  echo "iptables allow for openr TCP ports is already present";
else
  if [ $alter == "false" ]; then
    exit 1
  fi
  echo "adding iptables allow for openr TCP ports";
  $default iptables -A INPUT -p tcp --match multiport --dports 60000:60100 -j ACCEPT -m comment --comment "openr_v4_tcp_ports"
fi

#
# Open up UDP port for neighbor discovery using link local multicast
#

if $default ip6tables -nL INPUT --line-numbers | grep -P "openr_v6_udp_port"
then
  echo "ip6tables allow for openr UDP port is already present";
else
  if [ $alter == "false" ]; then
    exit 1
  fi
  echo "adding ip6tables allow for openr IPv6 UDP port";
  $default ip6tables -A INPUT -p udp --dport 6666 -j ACCEPT -m comment --comment "openr_v6_udp_port"
fi

exit 0
