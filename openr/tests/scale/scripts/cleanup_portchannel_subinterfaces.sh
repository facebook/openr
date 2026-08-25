#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# cleanup_portchannel_subinterfaces.sh - Remove dot1q routed sub-interfaces
# created by setup_portchannel_subinterfaces.sh on an Arista EOS port-channel.
#
# Companion to setup_portchannel_subinterfaces.sh: use it for manual emergency
# cleanup when a scale run dies before its teardown removes the sub-interfaces.
# Idempotent -- `no interface` on EOS (and `ip link delete` on Linux) are safe
# even when the sub-interface is already gone.
#
# Accepts either the EOS name (Port-Channel100511) or the Linux name (po100511).

set -e

usage() {
    echo "Usage: $0 <port_channel> <num_vlans> [start_vlan_id]"
    echo ""
    echo "Arguments:"
    echo "  port_channel    EOS or Linux port-channel name (e.g. Port-Channel100111 or po100111)"
    echo "  num_vlans       Number of sub-interfaces to remove"
    echo "  start_vlan_id   Starting VLAN ID, 1-4094 (default: 1)"
    echo ""
    echo "Example:"
    echo "  $0 Port-Channel100111 256 100   # remove Po100111.100 .. Po100111.355"
    exit 1
}

if [ $# -lt 2 ]; then
    usage
fi

PORT_CHANNEL="$1"
NUM_VLANS="$2"
START_VLAN="${3:-1}"

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root"
    exit 1
fi

# Validate VLAN range fits in dot1q space
if [ "$START_VLAN" -lt 1 ] || [ $((START_VLAN + NUM_VLANS - 1)) -gt 4094 ]; then
    echo "Error: VLAN range ${START_VLAN}..$((START_VLAN + NUM_VLANS - 1)) is outside dot1q 1-4094"
    exit 1
fi

# Normalize the port-channel to both its EOS name (Port-ChannelN, for FastCli)
# and its Linux name (poN, for netlink / ip commands).
case "$PORT_CHANNEL" in
    Port-Channel*) EOS_BASE="$PORT_CHANNEL"; LINUX_IF="po${PORT_CHANNEL#Port-Channel}" ;;
    po[0-9]*)      LINUX_IF="$PORT_CHANNEL"; EOS_BASE="Port-Channel${PORT_CHANNEL#po}" ;;
    *) echo "Error: '$PORT_CHANNEL' is not a port-channel name (expected Port-ChannelN or poN)"; exit 1 ;;
esac

END_VLAN=$((START_VLAN + NUM_VLANS - 1))
echo "Removing sub-interfaces ${EOS_BASE}.${START_VLAN} .. ${EOS_BASE}.${END_VLAN} (${NUM_VLANS} total)..."

# Remove EOS routed sub-interfaces via FastCli when available. `no interface` is
# idempotent, so this is safe even if some sub-interfaces are already gone.
if command -v FastCli >/dev/null 2>&1; then
    CLI_CMDS="configure"
    for i in $(seq 0 $((NUM_VLANS - 1))); do
        VLAN_ID=$((START_VLAN + i))
        CLI_CMDS="$CLI_CMDS
no interface ${EOS_BASE}.${VLAN_ID}"
    done
    CLI_CMDS="$CLI_CMDS
end"
    # Feed via stdin so a large batch never hits the Linux 128KB per-arg limit
    # (E2BIG); output discarded since 'no interface' on a missing sub-interface
    # is a benign parser error.
    printf '%s\n' "$CLI_CMDS" | FastCli -p 15 >/dev/null 2>&1 || true
    echo "  EOS: issued 'no interface' for ${NUM_VLANS} sub-interface(s) on ${EOS_BASE}."
fi

# Linux fallback / belt-and-suspenders: delete any leftover kernel VLAN netdevs
# (EOS normally removes these when the routed sub-interface is deleted).
LINUX_REMOVED=0
for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${LINUX_IF}.${VLAN_ID}"
    if ip link show "$VLAN_IF" >/dev/null 2>&1; then
        ip link delete "$VLAN_IF" 2>/dev/null || true
        LINUX_REMOVED=$((LINUX_REMOVED + 1))
    fi
done
echo "  Linux: deleted ${LINUX_REMOVED} lingering ${LINUX_IF}.<vlan> netdev(s)."

echo ""
echo "Cleanup complete for ${EOS_BASE} VLANs ${START_VLAN}..${END_VLAN}."
