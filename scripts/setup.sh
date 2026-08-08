#!/bin/bash
# Setup script for Ubuntu 22.04+ — auto-detects hardware RDMA, falls back to SoftRoCE.
# Usage: bash setup.sh
set -e

echo "=== Installing dependencies ==="
sudo apt-get update -y
sudo apt-get install -y \
    git cmake gcc g++ make \
    libibverbs-dev librdmacm-dev ibverbs-utils \
    rdma-core perftest \
    iproute2 \
    linux-modules-extra-$(uname -r)

echo "=== Enabling SoftRoCE (if no hardware RDMA) ==="
# Check if hardware RDMA device exists
if ibv_devinfo 2>/dev/null | grep -q "hca_id"; then
    echo "RDMA device already present (hardware or rxe), skipping SoftRoCE setup"
else
    echo "No RDMA device found, setting up SoftRoCE"
    sudo modprobe rdma_rxe
    # Detect primary non-loopback interface
    NETDEV=$(ip link show | awk '/^[0-9]+: / && !/lo:/ {gsub(":",""); print $2; exit}')
    echo "Adding SoftRoCE on $NETDEV"
    sudo rdma link add rxe0 type rxe netdev "$NETDEV"
fi

echo "=== Verifying RDMA setup ==="
# rdma_cm picks the device automatically based on routing to the destination IP,
# so no manual device selection / env var needed.
ibv_devinfo

echo "=== Getting the repo ==="
if git -C "$(dirname "$0")/.." rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
    echo "Already inside a checkout: $REPO_DIR"
    cd "$REPO_DIR"
    git pull
else
    cd ~
    if [ ! -d rdma-ai-infra ]; then
        git clone https://github.com/yueanz/rdma-ai-infra.git
    fi
    cd rdma-ai-infra
    git pull
fi

echo "=== Building ==="
cmake -B build && cmake --build build -j

echo ""
echo "=== Done ==="
echo "Binaries in $(pwd)/build/"
echo "  phase1_verbs/{lat_send_recv,lat_rdma_write,bw_rdma_write}"
echo "  phase2_transport/backend_compare"
echo "  phase3_collective/allreduce_bench"
echo "  phase4_kv_cache/{kv_server,kv_bench}"
echo ""
echo "Have 2 back-to-back RDMA NICs? Isolate them into namespaces (idempotent, safe to re-run):"
echo "  sudo bash scripts/setup_netns.sh"
echo ""
echo "Then run a quick verification (NOT 127.0.0.1 — no RDMA device on lo):"
echo "  Terminal 1: sudo ip netns exec ns1 ./build/phase1_verbs/lat_send_recv"
echo "  Terminal 2: sudo ip netns exec ns2 ./build/phase1_verbs/lat_send_recv 192.168.100.1"
