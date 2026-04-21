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

echo "=== Selecting RDMA device ==="
# On Azure MANA, prefer the transport device (no netdev) over the RoCE device.
# On SoftRoCE / single-device setups, just pick the only device.
RDMA_DEV=$(rdma dev show 2>/dev/null | awk '{print $2}' | while read dev; do
    sysfs="/sys/class/infiniband/$dev/ports/1/net"
    if [ ! -d "$sysfs" ]; then
        echo "$dev"
        break
    fi
done)
if [ -z "$RDMA_DEV" ]; then
    RDMA_DEV=$(rdma dev show 2>/dev/null | awk 'NR==1{print $2}')
fi
echo "Using RDMA device: $RDMA_DEV"
grep -q "^export RDMA_DEVICE=" ~/.bashrc && \
    sed -i "s|^export RDMA_DEVICE=.*|export RDMA_DEVICE=$RDMA_DEV|" ~/.bashrc || \
    echo "export RDMA_DEVICE=$RDMA_DEV" >> ~/.bashrc
export RDMA_DEVICE=$RDMA_DEV

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
