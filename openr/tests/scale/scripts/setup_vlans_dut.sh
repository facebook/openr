#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# setup_vlans_dut.sh - Setup VLAN interfaces on an Arista EOS DUT for OpenR scale testing
# Creates VLAN sub-interfaces using Linux ip commands on the DUT.
#
# This is the DUT-side counterpart to setup_vlans.sh (test server side).
# On EOS, use the Linux interface name (e.g., et4_15_1 for Ethernet4/15/1).
# Find it with: ip link show | grep et

set -e

usage() {
    echo "Usage: $0 <base_interface> <num_vlans> [start_vlan_id]"
    echo ""
    echo "Arguments:"
    echo "  base_interface   Linux interface name (e.g., et4_15_1 for EOS Ethernet4/15/1)"
    echo "  num_vlans        Number of VLAN interfaces to create"
    echo "  start_vlan_id    Starting VLAN ID (default: 1)"
    echo ""
    echo "Examples:"
    echo "  $0 et4_15_1 4        # Creates et4_15_1.1 through et4_15_1.4"
    echo "  $0 et1_1 8 100       # Creates et1_1.100 through et1_1.107"
    echo ""
    echo "Note: On EOS, find Linux interface names with: ip link show | grep et"
    exit 1
}

if [ $# -lt 2 ]; then
    usage
fi

BASE_IF="$1"
NUM_VLANS="$2"
START_VLAN="${3:-1}"

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root"
    exit 1
fi

# Check base interface exists
if ! ip link show "$BASE_IF" >/dev/null 2>&1; then
    echo "Error: Interface $BASE_IF does not exist"
    echo ""
    echo "Available interfaces:"
    ip -br link show | grep -E '^et' || ip -br link show
    exit 1
fi

# Load 8021q module if not loaded
if ! lsmod | grep -q 8021q; then
    echo "Loading 8021q kernel module..."
    modprobe 8021q
fi

# Ensure base interface is up
echo "Ensuring $BASE_IF is up..."
ip link set "$BASE_IF" up

# Convert Linux interface name to EOS name for FastCli (e.g., et4_15_1 -> Ethernet4/15/1)
linux_to_eos_name() {
    echo "$1" | sed -E 's/^et/Ethernet/' | sed 's/_/\//g'
}

# Configure EOS routed sub-interfaces if FastCli is available (Arista EOS)
if command -v FastCli >/dev/null 2>&1; then
    EOS_BASE=$(linux_to_eos_name "$BASE_IF")

    # Clean up existing EOS sub-interfaces first
    echo "Cleaning up existing EOS sub-interfaces on $EOS_BASE..."
    CLI_CMDS="configure"
    for i in $(seq 0 $((NUM_VLANS - 1))); do
        VLAN_ID=$((START_VLAN + i))
        CLI_CMDS="$CLI_CMDS
no interface ${EOS_BASE}.${VLAN_ID}"
    done
    CLI_CMDS="$CLI_CMDS
end"
    FastCli -p 15 -c "$CLI_CMDS" 2>/dev/null || true

    # Create fresh EOS routed sub-interfaces with IP addresses
    echo "Configuring EOS routed sub-interfaces on $EOS_BASE..."
    CLI_CMDS="configure"
    for i in $(seq 0 $((NUM_VLANS - 1))); do
        VLAN_ID=$((START_VLAN + i))
        V4_O2=$((VLAN_ID / 256))
        V4_O3=$((VLAN_ID % 256))
        V6_HEX=$(printf "%x" $VLAN_ID)
        CLI_CMDS="$CLI_CMDS
interface ${EOS_BASE}.${VLAN_ID}
  encapsulation dot1q vlan $VLAN_ID
  ip address 10.${V4_O2}.${V4_O3}.2/24
  ipv6 address fd00:0:0:${V6_HEX}::2/64
  ipv6 enable"
    done
    CLI_CMDS="$CLI_CMDS
end"
    FastCli -p 15 -c "$CLI_CMDS"
    echo "EOS sub-interfaces configured."
    echo ""
fi

echo ""
echo "Creating $NUM_VLANS VLAN interfaces on $BASE_IF (starting from VLAN $START_VLAN)..."
echo ""

CREATED_IFS=""

for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${BASE_IF}.${VLAN_ID}"

    # Remove existing VLAN interface if present
    if ip link show "$VLAN_IF" >/dev/null 2>&1; then
        echo "  Removing existing $VLAN_IF..."
        ip link delete "$VLAN_IF" 2>/dev/null || true
    fi

    # Create VLAN interface (skip if FastCli already created it via EOS sub-interface)
    if ip link show "$VLAN_IF" >/dev/null 2>&1; then
        echo "  $VLAN_IF already exists (created by EOS), skipping ip link add"
    else
        echo "  Creating $VLAN_IF (VLAN ID $VLAN_ID)..."
        ip link add link "$BASE_IF" name "$VLAN_IF" type vlan id "$VLAN_ID"
    fi

    # Bring up the interface
    ip link set "$VLAN_IF" up

    # Enable IPv6 (sysctl uses '/' as VLAN separator, not '.')
    SYSCTL_IF=$(echo "$VLAN_IF" | sed 's/\./\//g')
    sysctl -q -w "net.ipv6.conf.${SYSCTL_IF}.disable_ipv6=0" 2>/dev/null || true

    # Assign unique IPv4 address (DUT = .2)
    V4_O2=$((VLAN_ID / 256))
    V4_O3=$((VLAN_ID % 256))
    V4_ADDR="10.${V4_O2}.${V4_O3}.2"
    ip addr add "${V4_ADDR}/24" dev "$VLAN_IF" 2>/dev/null || true

    # Assign unique IPv6 address (ULA range, each VLAN gets its own /64)
    V6_SUFFIX=$(printf "%x" $VLAN_ID)
    V6_ADDR="fd00:0:0:${V6_SUFFIX}::2"
    ip -6 addr add "${V6_ADDR}/64" dev "$VLAN_IF" 2>/dev/null || true

    CREATED_IFS="${CREATED_IFS}${VLAN_IF},"
done

echo ""
echo "Created VLAN interfaces:"
for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${BASE_IF}.${VLAN_ID}"
    IF_INDEX=$(cat /sys/class/net/"$VLAN_IF"/ifindex 2>/dev/null || echo "?")
    V4_ADDR=$(ip -4 addr show "$VLAN_IF" | grep -oP 'inet \K[^\s]+' | head -1)
    V6_LINK=$(ip -6 addr show "$VLAN_IF" scope link | grep -oP 'fe80::[^\s/]+' | head -1)
    V6_GLOBAL=$(ip -6 addr show "$VLAN_IF" scope global | grep -oP 'fd00:[^\s/]+' | head -1)
    echo "  $VLAN_IF  (ifIndex=$IF_INDEX, IPv4=$V4_ADDR, IPv6=$V6_GLOBAL, link-local=$V6_LINK)"
done

# Print interface list for --interfaces flag
INTERFACE_LIST="${CREATED_IFS%,}"  # Remove trailing comma
echo ""
echo "Use with scale_test_server:"
echo "  --interfaces=${INTERFACE_LIST}"
echo ""

# Verify Spark multicast group is joinable
echo "Verifying multicast setup..."
SPARK_MCAST="ff02::1"

for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${BASE_IF}.${VLAN_ID}"

    # Try to join multicast group (this will fail silently if already joined)
    # The actual join happens in RealSparkIo
    if ip maddr show dev "$VLAN_IF" | grep -q "$SPARK_MCAST"; then
        echo "  $VLAN_IF: multicast OK"
    else
        echo "  $VLAN_IF: multicast ready (will join on demand)"
    fi
done

echo ""
echo "DUT VLAN setup complete!"
echo ""
echo "To remove these VLANs later, run:"
echo "  for i in \$(seq $START_VLAN $((START_VLAN + NUM_VLANS - 1))); do sudo ip link delete ${BASE_IF}.\$i; done"
