#!/bin/bash
# Setup script for Ubuntu — auto-detects hardware RDMA, falls back to SoftRoCE.
# Production target is Alibaba Cloud ECS with eRDMA; works on any Ubuntu 22.04+.
# Usage: bash setup.sh
set -e

echo "=== Installing dependencies ==="
sudo apt-get update -y
sudo apt-get install -y \
    git cmake gcc g++ make \
    libibverbs-dev librdmacm-dev ibverbs-utils \
    rdma-core \
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

echo "=== Cloning repo ==="
cd ~
if [ ! -d rdma-ai-infra ]; then
    git clone https://github.com/yueanz/rdma-ai-infra.git
fi
cd rdma-ai-infra
git pull

echo "=== Building ==="
cmake -B build && cmake --build build -j

echo ""
echo "=== Done ==="
echo "Binaries in ~/rdma-ai-infra/build/"
echo "  phase1_verbs/{lat_send_recv,lat_rdma_write,bw_rdma_write}"
echo "  phase2_transport/backend_compare"
echo "  phase3_collective/allreduce_bench"
echo "  phase4_kv_cache/{kv_server,kv_bench}"
echo ""
echo "Run a quick verification (replace <ip> with the rxe-bound NIC's IP, NOT 127.0.0.1):"
echo "  Terminal 1: ./build/phase1_verbs/lat_send_recv"
echo "  Terminal 2: ./build/phase1_verbs/lat_send_recv <ip>"
