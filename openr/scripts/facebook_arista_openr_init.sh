#!/bin/bash

#
# Copyright (c) 2014-present, Facebook, Inc.
#

#
# This script holds set of commands that needs to be performed when arista
# device reboots. It will be invoked by fabric-toolkit on every reboot to
# ensure open/r and it's dependencies.
#

OPENR_ROOT="/mnt/drive/persistent/usr/local/fbprojects/openr"

# Create symlink for breeze - useful for human debugging
/bin/ln -sf "$OPENR_ROOT/usr/sbin/breeze" /sbin/breeze


# Create symlink for AristaFibAgent mount profile - necessary for daemon to run
/bin/ln -sf \
  "$OPENR_ROOT/usr/lib/SysdbMountProfiles/AristaFibAgent" \
  /usr/lib/SysdbMountProfiles/AristaFibAgent
