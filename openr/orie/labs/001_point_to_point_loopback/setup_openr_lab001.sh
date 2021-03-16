#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

#
# Simple Script to setup and destroy Open/R Lab
# Lab 001: Point to Point
#

create_namespace() {
    if [ ! -e "/run/netns/$1" ]; then
        "$IP" netns add "$1"
    else
        echo "Namespace $1 already exists ... "
    fi
}


delete_namespace() {
    if [ ! -e "/run/netns/$1" ]; then
        echo "Namespace $1 does not exist ... "
    else
        "$IP" netns delete "$1"
    fi
}


get_prefix() {
    case "$1" in
        "openr_left_oob1")
            echo "fd00:69::2/64"
            ;;
        "openr_left_left0")
            echo "fd00::1/64"
            ;;
        "openr_left_lo")
            echo "fd00:1::1/64"
            ;;
        "openr_right_oob2")
            echo "fd00:69::3/64"
            ;;
        "openr_right_right0")
            echo "fd00::2/64"
            ;;
        "openr_right_lo")
            echo "fd00:2::1/64"
            ;;
    esac
}


CONFIG_DIR="/tmp/orie_configs"
IP=/usr/sbin/ip
NAMESPACE1="openr_left"
NAMESPACE2="openr_right"
OPENR_OOB_INT="openr_oob"
OPENR_OOB_SUBNET="fd00:69::1/64"


if [ "$1" == "check" ]; then
    for namespace in $NAMESPACE1 $NAMESPACE2
    do
        echo "--> Checking $namespace"
        echo "## Addresses"
        "$IP" netns exec $namespace "$IP" addr show
        echo "## Routes"
        "$IP" netns exec $namespace "$IP" -6 route show
    done
    exit 0
elif [ "$1" == "delete" ]; then
    for namespace in $NAMESPACE1 $NAMESPACE2
    do
        echo "--> Deleting $namespace"
        delete_namespace "$namespace"
    done

    echo "--> Deleting OOB interface"
    "$IP" link del "$OPENR_OOB_INT"
    exit 0
elif [ "$1" == "create" ]; then
    echo "--> Creating OOB bridge with IP $OPENR_OOB_SUBNET on $OPENR_OOB_INT via eth0"
    "$IP" link add "$OPENR_OOB_INT" link eth0 type macvlan mode bridge
    "$IP" link set up "$OPENR_OOB_INT"
    "$IP" addr add "$OPENR_OOB_SUBNET" dev "$OPENR_OOB_INT"
    echo "--> Creating p2p veths"
    "$IP" link add left0 type veth peer name right0
    "$IP" link add right0 type veth peer name left0

    netns_oob_index=1
    for namespace in $NAMESPACE1 $NAMESPACE2
    do
        echo "--> Setting up $namespace"
        create_namespace "$namespace"

        # Setup OOB Interface for breeze to use from global namespace
        "$IP" link add "oob${netns_oob_index}" link eth0 type macvlan mode bridge
        "$IP" link set "oob${netns_oob_index}" netns "$namespace"
        "$IP" netns exec "$namespace" "$IP" link set up "oob${netns_oob_index}"
        "$IP" netns exec "$namespace" "$IP" addr add "$(get_prefix "${namespace}_oob${netns_oob_index}")" dev "oob${netns_oob_index}"
        netns_oob_index=$((netns_oob_index+1))

        # Enable + address loopback for netns
        "$IP" netns exec "$namespace" ip link set up lo
        "$IP" netns exec "$namespace" ip addr add "$(get_prefix ${namespace}_lo)" dev lo

        # Create a veth to other netns
        side=$(echo $namespace | cut -d '_' -f 2)
        netns_interface="${side}0"

        "$IP" link set "$netns_interface" netns "$namespace"

        "$IP" netns exec "$namespace" "$IP" link set up "$netns_interface"
        "$IP" netns exec "$namespace" "$IP" addr add "$(get_prefix "${namespace}_${netns_interface}")" dev "$netns_interface"
    done
    exit 0
elif [ "$1" == "copy-conf" ]; then
    if [ "$USER" == "root" ]; then
        echo "!!-> Don't copy as root ..."
        exit 1
    fi
    # Copy configs to /tmp
    # Have to copy due to eden behviours
    config_src="$(ls $HOME/fbsource/fbcode/openr/orie/labs/001_point_to_point_loopback/*.conf | tr '\n' ' ')"
    echo "--> Configuration being copied: $config_src to $CONFIG_DIR"
    mkdir -pv "$CONFIG_DIR"
    sudo chown "$USER" "$CONFIG_DIR"
    cp -v "$config_src" "$CONFIG_DIR"
    exit $?
elif [ "$1" == "start" ]; then
    # Automagically selects config file depending on your netns
    openr_binary="$HOME/fbsource/fbcode/buck-out/gen/openr/openr"
    config="$CONFIG_DIR/$(ip netns identify).conf"
    echo "--> Starting Open/R using --config=$config"
    "$openr_binary" --config="$config" --logtostderr --v=2 --enable_secure_thrift_server=false |& tee "/tmp/$(ip netns identify).log"
    exit $?
fi

echo "!!--> ERROR: Specify an option: create|delete|check|copy-conf|start !!"
exit 69
