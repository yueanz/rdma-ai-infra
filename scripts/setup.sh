#!/bin/bash
# Setup script for Ubuntu (Azure VM)
# Usage: bash setup.sh
set -e

echo "=== Installing dependencies ==="
sudo apt-get update -y
sudo apt-get install -y \
    git cmake gcc g++ make \
    libibverbs-dev ibverbs-utils \
    rdma-core \
    ibacm infiniband-diags \
    iproute2 \
    linux-modules-extra-$(uname -r)

echo "=== Enabling SoftRoCE (if no hardware RDMA) ==="
# Check if hardware RDMA device exists
if ibv_devinfo 2>/dev/null | grep -q "hca_id"; then
    echo "Hardware RDMA device found, skipping SoftRoCE setup"
else
    echo "No hardware RDMA found, setting up SoftRoCE"
    sudo modprobe rdma_rxe
    # Detect primary non-loopback interface
    NETDEV=$(ip link show | awk '/^[0-9]+: / && !/lo:/ {gsub(":",""); print $2; exit}')
    echo "Adding SoftRoCE on $NETDEV"
    sudo rdma link add rxe0 type rxe netdev "$NETDEV"
fi

echo "=== Verifying RDMA device ==="
ibv_devinfo

echo "=== Cloning repo ==="
cd ~
if [ ! -d rdma-ai-infra ]; then
    git clone https://github.com/yueanz/rdma-ai-infra.git
fi
cd rdma-ai-infra
git pull

echo "=== Building ==="
mkdir -p build && cd build
cmake .. && make -j$(nproc)

echo "=== Testing RDMA loopback ==="
cd ~/rdma-ai-infra/build/phase1_verbs
./lat_send_recv &
SERVER_PID=$!
sleep 0.5
./lat_send_recv 127.0.0.1
wait $SERVER_PID

echo ""
echo "=== Done ==="
echo "Binaries in ~/rdma-ai-infra/build/"
echo "  phase1_verbs/lat_send_recv"
echo "  phase1_verbs/lat_rdma_write"
echo "  phase1_verbs/bw_rdma_write"
echo "  phase2_transport/backend_compare"
