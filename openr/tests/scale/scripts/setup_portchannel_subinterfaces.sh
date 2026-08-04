#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# setup_portchannel_subinterfaces.sh - Create dot1q routed sub-interfaces on an
# Arista EOS port-channel for OpenR scale testing.
#
# This is the port-channel counterpart of setup_vlans.sh / setup_vlans_dut.sh:
# instead of carving VLANs off a single physical interface, it stacks N routed
# sub-interfaces on a LAG (port-channel). Each sub-interface is an independent
# L3 link, so OpenR forms one additional adjacency per sub-interface over the
# same port-channel bundle.
#
# One script serves both ends of the link; the host octet selects the side:
#   - helper / test-server side -> host octet 1  (addresses end in .1 / ::1)
#   - DUT side                  -> host octet 2  (addresses end in .2 / ::2)
# Run it on BOTH ends with the SAME num_vlans / start_vlan so the tags line up.
#
# Accepts either the EOS name (Port-Channel100511) or the Linux name (po100511).
# Find port-channels with: show port-channel summary   (EOS)
#                          ip -br link show | grep '^po' (Linux)

set -e

usage() {
    echo "Usage: $0 <port_channel> <num_vlans> [start_vlan_id] [host_octet]"
    echo ""
    echo "Arguments:"
    echo "  port_channel    EOS or Linux port-channel name (e.g. Port-Channel100511 or po100511)"
    echo "  num_vlans       Number of sub-interfaces to create"
    echo "  start_vlan_id   Starting VLAN ID, 1-4094 (default: 1)"
    echo "  host_octet      Host id for addressing: 1 = helper/test-server, 2 = DUT (default: 2)"
    echo ""
    echo "Examples:"
    echo "  $0 Port-Channel100111 256 100 2   # DUT: 256 sub-ifs on Po100111, VLANs 100-355, .2 addrs"
    echo "  $0 po100511 256 100 1             # helper: matching sub-ifs on po100511, .1 addrs"
    echo ""
    echo "Notes:"
    echo "  - Run on BOTH ends with the same num_vlans/start_vlan (helper=1, DUT=2)."
    echo "  - dot1q allows VLAN IDs 1-4094; the platform sub-interface cap may be lower"
    echo "    (check 'show hardware capacity' -> SubInterfaces)."
    exit 1
}

if [ $# -lt 2 ]; then
    usage
fi

PORT_CHANNEL="$1"
NUM_VLANS="$2"
START_VLAN="${3:-1}"
HOST_OCTET="${4:-2}"

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root"
    exit 1
fi

# Validate host octet
if [ "$HOST_OCTET" != "1" ] && [ "$HOST_OCTET" != "2" ]; then
    echo "Error: host_octet must be 1 (helper/test-server) or 2 (DUT), got '$HOST_OCTET'"
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

# Check base interface exists (Linux side)
if ! ip link show "$LINUX_IF" >/dev/null 2>&1; then
    echo "Error: Interface $LINUX_IF does not exist"
    echo ""
    echo "Available port-channels:"
    ip -br link show | grep -E '^po[0-9]' || ip -br link show
    exit 1
fi

# Load 8021q module if not loaded
if ! lsmod | grep -q 8021q; then
    echo "Loading 8021q kernel module..."
    modprobe 8021q
fi

# Ensure base interface is up
echo "Ensuring $LINUX_IF ($EOS_BASE) is up..."
ip link set "$LINUX_IF" up

# Configure EOS routed sub-interfaces if FastCli is available (Arista EOS).
# EOS creates the matching Linux netdev, so the ip-link step below is skipped.
if command -v FastCli >/dev/null 2>&1; then
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
  ip address 10.${V4_O2}.${V4_O3}.${HOST_OCTET}/24
  ipv6 address fd00:0:0:${V6_HEX}::${HOST_OCTET}/64
  ipv6 enable"
    done
    CLI_CMDS="$CLI_CMDS
end"
    FastCli -p 15 -c "$CLI_CMDS"
    echo "EOS sub-interfaces configured."
    echo ""
fi

echo ""
echo "Creating $NUM_VLANS sub-interfaces on $LINUX_IF (VLANs $START_VLAN..$((START_VLAN + NUM_VLANS - 1)), host octet $HOST_OCTET)..."
echo ""

CREATED_IFS=""

for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${LINUX_IF}.${VLAN_ID}"

    # Create VLAN interface only if EOS/FastCli did not already create it
    if ip link show "$VLAN_IF" >/dev/null 2>&1; then
        echo "  $VLAN_IF already exists (created by EOS), skipping ip link add"
    else
        echo "  Creating $VLAN_IF (VLAN ID $VLAN_ID)..."
        ip link add link "$LINUX_IF" name "$VLAN_IF" type vlan id "$VLAN_ID"
    fi

    # Bring up the interface
    ip link set "$VLAN_IF" up

    # Enable IPv6 (sysctl uses '/' as VLAN separator, not '.')
    SYSCTL_IF=$(echo "$VLAN_IF" | sed 's/\./\//g')
    sysctl -q -w "net.ipv6.conf.${SYSCTL_IF}.disable_ipv6=0" 2>/dev/null || true

    # Assign unique IPv4 address (per-VLAN /24; host octet selects the side)
    V4_O2=$((VLAN_ID / 256))
    V4_O3=$((VLAN_ID % 256))
    V4_ADDR="10.${V4_O2}.${V4_O3}.${HOST_OCTET}"
    ip addr add "${V4_ADDR}/24" dev "$VLAN_IF" 2>/dev/null || true

    # Assign unique IPv6 address (ULA range, each VLAN gets its own /64)
    V6_SUFFIX=$(printf "%x" $VLAN_ID)
    V6_ADDR="fd00:0:0:${V6_SUFFIX}::${HOST_OCTET}"
    ip -6 addr add "${V6_ADDR}/64" dev "$VLAN_IF" 2>/dev/null || true

    CREATED_IFS="${CREATED_IFS}${VLAN_IF},"
done

echo ""
echo "Created sub-interfaces:"
for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${LINUX_IF}.${VLAN_ID}"
    IF_INDEX=$(cat /sys/class/net/"$VLAN_IF"/ifindex 2>/dev/null || echo "?")
    V4_ADDR=$(ip -4 addr show "$VLAN_IF" | grep -oP 'inet \K[^\s]+' | head -1)
    V6_GLOBAL=$(ip -6 addr show "$VLAN_IF" scope global | grep -oP 'fd00:[^\s/]+' | head -1)
    V6_LINK=$(ip -6 addr show "$VLAN_IF" scope link | grep -oP 'fe80::[^\s/]+' | head -1)
    echo "  $VLAN_IF  (ifIndex=$IF_INDEX, IPv4=$V4_ADDR, IPv6=$V6_GLOBAL, link-local=$V6_LINK)"
done

# Print interface list for the scale_test_server --interfaces flag
INTERFACE_LIST="${CREATED_IFS%,}"  # Remove trailing comma
echo ""
echo "Use with scale_test_server:"
echo "  --interfaces=${INTERFACE_LIST}"
echo ""

# Verify Spark multicast group is joinable (Spark discovers neighbors via ff02::1)
echo "Verifying multicast setup..."
SPARK_MCAST="ff02::1"
for i in $(seq 0 $((NUM_VLANS - 1))); do
    VLAN_ID=$((START_VLAN + i))
    VLAN_IF="${LINUX_IF}.${VLAN_ID}"
    if ip maddr show dev "$VLAN_IF" | grep -q "$SPARK_MCAST"; then
        echo "  $VLAN_IF: multicast OK"
    else
        echo "  $VLAN_IF: multicast ready (will join on demand)"
    fi
done

echo ""
# Port ranges for FakeKvStore Thrift servers (see ScaleTestServer.cpp
# --fake_kvstore_base_port, default 3000). Override via env vars if running
# with more neighbors or a different base port.
INPUT_PORT_RANGE="${INPUT_PORT_RANGE:-3000:3500}"
OUTPUT_PORT_RANGE="${OUTPUT_PORT_RANGE:-3000:3100}"
echo "Configuring ip6tables to allow OpenR scale test traffic..."
echo "  INPUT dport range:  $INPUT_PORT_RANGE"
echo "  OUTPUT sport range: $OUTPUT_PORT_RANGE"
if ! ip6tables -C INPUT -p tcp --dport "$INPUT_PORT_RANGE" -j ACCEPT 2>/dev/null; then
    ip6tables -I INPUT 1 -p tcp --dport "$INPUT_PORT_RANGE" -j ACCEPT
    echo "  Added INPUT rule for tcp dpts $INPUT_PORT_RANGE"
else
    echo "  INPUT rule for tcp dpts $INPUT_PORT_RANGE already present"
fi
if ! ip6tables -C OUTPUT -p tcp --sport "$OUTPUT_PORT_RANGE" -j ACCEPT 2>/dev/null; then
    ip6tables -I OUTPUT 1 -p tcp --sport "$OUTPUT_PORT_RANGE" -j ACCEPT
    echo "  Added OUTPUT rule for tcp spts $OUTPUT_PORT_RANGE"
else
    echo "  OUTPUT rule for tcp spts $OUTPUT_PORT_RANGE already present"
fi

echo ""
echo "Port-channel sub-interface setup complete!"
echo ""
echo "OpenR: ensure include_interface_regexes matches these sub-interfaces"
echo "  (e.g. 'po[0-9]{6}\\.[0-9]+\$') in the LOADED config (/etc/openr_config),"
echo "  not just a persistent copy. New interfaces are picked up without a restart"
echo "  once the regex is present."
echo ""
echo "To remove these sub-interfaces later:"
if command -v FastCli >/dev/null 2>&1; then
    echo "  In EOS config mode, for each VLAN ${START_VLAN}..$((START_VLAN + NUM_VLANS - 1)): no interface ${EOS_BASE}.<vlan>"
else
    echo "  for i in \$(seq $START_VLAN $((START_VLAN + NUM_VLANS - 1))); do sudo ip link delete ${LINUX_IF}.\$i; done"
fi
