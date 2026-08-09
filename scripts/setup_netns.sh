#!/bin/bash
# Bring the two back-to-back RDMA NICs from a cold boot to ready-to-benchmark:
# links up, each device isolated in its own network namespace (ns1 / ns2) with
# an IP, so a single host behaves like two independent RoCEv2 peers.
#
# Idempotent: safe to re-run, including after a reboot. Nothing here survives a
# reboot — namespaces live in tmpfs, netns-exclusive mode resets to shared, and
# RDMA device names are not stable either (mlx5_0 one boot, rocep3s0 the next,
# depending on rdma-core's udev rename policy). So every name is re-discovered
# rather than hardcoded.
#
# Usage: sudo bash scripts/setup_netns.sh
set -euo pipefail

NS1=ns1
NS2=ns2
IP1=192.168.100.1
IP2=192.168.100.2
LINK_WAIT_SEC=15

log() { echo "=== $* ==="; }

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root (sudo bash $0)" >&2
    exit 1
fi

# The physical link only comes up after autonegotiation, which takes seconds on
# 100GbE. Without this the ping below races the link and fails spuriously.
wait_link_up() {
    local ns=$1 dev=$2 deadline=$((SECONDS + LINK_WAIT_SEC))
    while [ $SECONDS -lt $deadline ]; do
        if ip netns exec "$ns" rdma link 2>/dev/null |
           grep -q "^link ${dev}/.*state ACTIVE physical_state LINK_UP"; then
            return 0
        fi
        sleep 0.2
    done
    echo "$dev in $ns did not reach ACTIVE/LINK_UP within ${LINK_WAIT_SEC}s" >&2
    ip netns exec "$ns" rdma link >&2 || true
    return 1
}

# The GID index the raw verbs path will pick is the first RoCE v2 entry, so
# print the table — it is the usual suspect when INIT->RTR fails.
show_gids() {
    local ns=$1
    ip netns exec "$ns" bash -c '
        for g in /sys/class/infiniband/*/ports/1/gids/*; do
            [ -e "$g" ] || continue
            gid=$(cat "$g")
            [ "$gid" = "0000:0000:0000:0000:0000:0000:0000:0000" ] && continue
            idx=$(basename "$g")
            port_dir=$(dirname "$(dirname "$g")")
            printf "  idx=%-3s %-40s %s\n" \
                "$idx" "$gid" "$(cat "$port_dir/gid_attrs/types/$idx" 2>/dev/null)"
        done'
}

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
    if rdma system show | grep -q "netns exclusive"; then
        echo "Already exclusive"
    else
        rdma system set netns exclusive
    fi

    log "Creating namespaces (if missing)"
    ip netns add "$NS1" 2>/dev/null && echo "created $NS1" || echo "$NS1 already exists"
    ip netns add "$NS2" 2>/dev/null && echo "created $NS2" || echo "$NS2 already exists"

    log "Assigning devices and netdevs to namespaces"
    rdma dev set "$DEV1" netns "$NS1"
    rdma dev set "$DEV2" netns "$NS2"
    # The netdev does not always follow its RDMA device, so move it explicitly.
    ip link set "$NETDEV1" netns "$NS1" 2>/dev/null || true
    ip link set "$NETDEV2" netns "$NS2" 2>/dev/null || true
elif [ "${#DEVICES[@]}" -eq 0 ]; then
    echo "None in the root netns — expecting a previous run to have placed them."
else
    echo "Expected 0 or 2 RDMA devices in the root netns, found ${#DEVICES[@]}: ${DEVICES[*]}" >&2
    exit 1
fi

log "Verifying device placement"
for ns in "$NS1" "$NS2"; do
    n=$(ip netns exec "$ns" rdma dev show | wc -l)
    if [ "$n" -ne 1 ]; then
        echo "$ns holds $n RDMA devices, expected exactly 1" >&2
        exit 1
    fi
    ip netns exec "$ns" rdma dev show | sed 's/^/  /'
done

DEV1=$(ip netns exec "$NS1" rdma dev show | awk -F': ' '{print $2}')
DEV2=$(ip netns exec "$NS2" rdma dev show | awk -F': ' '{print $2}')
NETDEV1=$(ip netns exec "$NS1" ip -o link show | awk -F': ' '$2 != "lo" {print $2; exit}')
NETDEV2=$(ip netns exec "$NS2" ip -o link show | awk -F': ' '$2 != "lo" {print $2; exit}')

log "Bringing interfaces up and assigning IPs"
ip netns exec "$NS1" ip link set lo up
ip netns exec "$NS2" ip link set lo up
ip netns exec "$NS1" ip addr add "$IP1/24" dev "$NETDEV1" 2>/dev/null || true
ip netns exec "$NS2" ip addr add "$IP2/24" dev "$NETDEV2" 2>/dev/null || true
ip netns exec "$NS1" ip link set "$NETDEV1" up
ip netns exec "$NS2" ip link set "$NETDEV2" up
echo "$NS1: $DEV1 / $NETDEV1 / $IP1"
echo "$NS2: $DEV2 / $NETDEV2 / $IP2"

log "Waiting for both links to reach ACTIVE"
wait_link_up "$NS1" "$DEV1"
wait_link_up "$NS2" "$DEV2"
echo "both up"

log "GID tables"
echo "$NS1 ($DEV1):"; show_gids "$NS1"
echo "$NS2 ($DEV2):"; show_gids "$NS2"

log "Verifying connectivity"
ip netns exec "$NS1" ping -c 2 -W 2 "$IP2"

echo ""
log "Ready"
echo "  sudo ip netns exec $NS1 ./build/phase1_verbs/lat_send_recv --conn verbs"
echo "  sudo ip netns exec $NS2 ./build/phase1_verbs/lat_send_recv $IP1 --conn verbs"
