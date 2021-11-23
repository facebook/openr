#!/bin/bash
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

#
# Simple Script to copy configs + start openr
#


CONFIG_DIR="/tmp/orie_configs"


if [ "$1" == "copy-conf" ]; then
    # No need to copy cofigs as root
    if [ "$USER" == "root" ]; then
        echo "!!-> Don't copy as root ..."
        exit 1
    fi

    # Copy configs to /tmp
    # Have to copy due to eden behviours

    config_dir="$HOME/fbsource/fbcode/openr/orie/labs/$2"
    if [ ! -e "$config_dir" ]; then
        echo "!!-> $config_dir does not exist"
        exit 2
    fi

    config_src="$(find "$config_dir" -name '*.conf' | tr '\n' ' ')"
    echo "--> Configuration being copied: $config_src to $CONFIG_DIR"
    mkdir -pv "$CONFIG_DIR"
    sudo chown "$USER" "$CONFIG_DIR"
    # shellcheck disable=SC2086
    cp -v $config_src "$CONFIG_DIR"
    exit $?
elif [ "$1" == "start" ]; then
    # Automagically selects config file depending on your netns name
    openr_binary="$HOME/fbsource/fbcode/buck-out/gen/openr/openr"
    config="$CONFIG_DIR/$(ip netns identify).conf"
    echo "--> Starting Open/R using --config=$config"
    "$openr_binary" \
        --config="$config" \
        --logtostderr \
        --v=2 \
        --enable_secure_thrift_server=false |& tee "/tmp/$(ip netns identify).log"
    exit $?
fi

echo "!!--> ERROR: Specify an option: copy-conf|start !!"
exit 69
