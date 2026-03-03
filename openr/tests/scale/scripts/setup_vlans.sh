#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# setup_vlans.sh - Setup VLAN interfaces for OpenR scale testing
# Creates VLAN interfaces on a test server for connecting to a DUT via VLAN trunk.

set -e

usage() {
    echo "Usage: $0 <base_interface> <num_vlans> [start_vlan_id]"
    echo ""
    echo "Arguments:"
    echo "  base_interface   Physical interface (e.g., eth0, ens3)"
    echo "  num_vlans        Number of VLAN interfaces to create"
    echo "  start_vlan_id    Starting VLAN ID (default: 1)"
    echo ""
    echo "Examples:"
    echo "  $0 eth0 4        # Creates eth0.1 through eth0.4"
    echo "  $0 eth1 8 100    # Creates eth1.100 through eth1.107"
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
        ip link delete "$VLAN_IF"
    fi

    # Create VLAN interface
    echo "  Creating $VLAN_IF (VLAN ID $VLAN_ID)..."
    ip link add link "$BASE_IF" name "$VLAN_IF" type vlan id "$VLAN_ID"

    # Assign a link-local IPv6 address
    # Use a deterministic address based on VLAN ID
    V6_SUFFIX=$(printf "%04x" $VLAN_ID)
    V6_ADDR="fe80::${V6_SUFFIX}"

    # Bring up the interface
    ip link set "$VLAN_IF" up

    # Add IPv6 address (for Spark multicast)
    # The kernel will auto-assign a link-local address, but we add one for consistency
    ip -6 addr add "${V6_ADDR}/64" dev "$VLAN_IF" 2>/dev/null || true

    # Enable IPv6 multicast
    sysctl -q -w "net.ipv6.conf.${VLAN_IF}.disable_ipv6=0" 2>/dev/null || true

    CREATED_IFS="${CREATED_IFS}${VLAN_IF},"
done

echo ""
echo "Created VLAN interfaces:"
for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${BASE_IF}.${VLAN_ID}"
    IF_INDEX=$(cat /sys/class/net/"$VLAN_IF"/ifindex 2>/dev/null || echo "?")
    V6_ADDR=$(ip -6 addr show "$VLAN_IF" scope link | grep -oP 'fe80::[^\s/]+' | head -1)
    echo "  $VLAN_IF  (ifIndex=$IF_INDEX, IPv6=$V6_ADDR)"
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
echo "VLAN setup complete!"
echo ""
echo "To remove these VLANs later, run:"
echo "  for i in \$(seq $START_VLAN $((START_VLAN + NUM_VLANS - 1))); do sudo ip link delete ${BASE_IF}.\$i; done"
