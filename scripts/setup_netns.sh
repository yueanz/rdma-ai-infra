#!/bin/bash
# Isolate the two back-to-back RDMA NICs into separate network namespaces
# (ns1 / ns2) so a single host behaves like two independent RDMA peers over
# RoCEv2. See README.md > "Hardware & Test Environment" for the manual
# walkthrough this script automates.
#
# Idempotent: safe to re-run, including after a reboot. RDMA device names are
# NOT stable across reboots (e.g. mlx5_0 <-> rocep3s0, depending on
# rdma-core's udev rename policy at boot time), so this script re-discovers
# device and netdev names every run instead of hardcoding them.
#
# Usage: sudo bash scripts/setup_netns.sh
set -euo pipefail

NS1=ns1
NS2=ns2
IP1=192.168.100.1
IP2=192.168.100.2

log() { echo "=== $* ==="; }

log "Locating RDMA devices in the root netns"
mapfile -t DEVICES < <(rdma dev show | awk -F': ' '{print $2}')

if [ "${#DEVICES[@]}" -eq 2 ]; then
    DEV1="${DEVICES[0]}"
    DEV2="${DEVICES[1]}"
    echo "Found: $DEV1, $DEV2"

    log "Resolving netdev for each device (before moving)"
    NETDEV1=$(rdma link | grep "^link ${DEV1}/" | grep -oP 'netdev \K\S+')
    NETDEV2=$(rdma link | grep "^link ${DEV2}/" | grep -oP 'netdev \K\S+')
    echo "$DEV1 -> $NETDEV1"
    echo "$DEV2 -> $NETDEV2"

    log "Ensuring netns-exclusive mode"
    if ! rdma system show | grep -q "netns exclusive"; then
        rdma system set netns exclusive
    else
        echo "Already exclusive"
    fi

    log "Creating namespaces (if missing)"
    ip netns add "$NS1" 2>/dev/null && echo "created $NS1" || echo "$NS1 already exists"
    ip netns add "$NS2" 2>/dev/null && echo "created $NS2" || echo "$NS2 already exists"

    log "Assigning devices and netdevs to namespaces"
    rdma dev set "$DEV1" netns "$NS1"
    rdma dev set "$DEV2" netns "$NS2"
    ip link set "$NETDEV1" netns "$NS1" 2>/dev/null || true
    ip link set "$NETDEV2" netns "$NS2" 2>/dev/null || true
elif [ "${#DEVICES[@]}" -eq 0 ]; then
    echo "No free RDMA devices in the root netns — assuming they're already assigned to $NS1/$NS2 from a previous run."
else
    echo "Expected 0 or 2 RDMA devices in the root netns, found ${#DEVICES[@]}: ${DEVICES[*]}" >&2
    exit 1
fi

log "Verifying device placement"
ip netns exec "$NS1" rdma dev show
ip netns exec "$NS2" rdma dev show

log "Configuring IPs"
NETDEV1=$(ip netns exec "$NS1" ip -o link show | awk -F': ' '$2 != "lo" {print $2; exit}')
NETDEV2=$(ip netns exec "$NS2" ip -o link show | awk -F': ' '$2 != "lo" {print $2; exit}')

ip netns exec "$NS1" ip link set lo up
ip netns exec "$NS2" ip link set lo up

ip netns exec "$NS1" ip addr add "$IP1/24" dev "$NETDEV1" 2>/dev/null || true
ip netns exec "$NS1" ip link set "$NETDEV1" up

ip netns exec "$NS2" ip addr add "$IP2/24" dev "$NETDEV2" 2>/dev/null || true
ip netns exec "$NS2" ip link set "$NETDEV2" up

log "Verifying connectivity"
ip netns exec "$NS1" ping -c 2 "$IP2"

echo ""
log "Done"
echo "Server example: sudo ip netns exec $NS1 <binary>"
echo "Client example: sudo ip netns exec $NS2 <binary> $IP1"
