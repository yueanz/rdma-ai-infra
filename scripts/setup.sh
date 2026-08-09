#!/bin/bash
# Install dependencies and build. Ubuntu 22.04+.
# Usage: bash scripts/setup.sh
set -e

echo "=== Installing dependencies ==="
sudo apt-get update -y
sudo apt-get install -y \
    git cmake gcc g++ make \
    libibverbs-dev librdmacm-dev ibverbs-utils \
    rdma-core perftest \
    iproute2

echo "=== RDMA devices ==="
# rdma_cm picks the device from the route to the destination IP, and the raw
# verbs path picks the one device visible in its namespace, so nothing here
# needs configuring — this is just a check that the cards are present.
rdma link || echo "no RDMA devices — check that the NICs are seated and the driver loaded"

echo "=== Building ==="
cd "$(cd "$(dirname "$0")/.." && pwd)"
cmake -B build && cmake --build build -j

echo ""
echo "=== Done ==="
echo "Binaries in $(pwd)/build/"
echo "  phase1_verbs/{lat_send_recv,lat_rdma_write,bw_rdma_write}"
echo "  phase2_transport/backend_compare"
echo "  phase3_collective/allreduce_bench"
echo "  phase4_kv_cache/{kv_server,kv_bench}"
echo ""
echo "Two back-to-back NICs? Put them in their own namespaces first:"
echo "  sudo bash scripts/setup_netns.sh"
echo ""
echo "Then, from two terminals (NOT 127.0.0.1 — there is no RDMA device on lo):"
echo "  sudo ip netns exec ns1 ./build/phase1_verbs/lat_send_recv"
echo "  sudo ip netns exec ns2 ./build/phase1_verbs/lat_send_recv 192.168.100.1"
