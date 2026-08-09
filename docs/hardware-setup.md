# Hardware setup notes

Everything `scripts/setup_netns.sh` automates, and why each step is there.
Most of it is a note-to-self about something that bit during bring-up.

## RoCEv2 bring-up and network namespace isolation

Both ConnectX-4 ports are wired directly to each other. To make this single host behave like two independent hosts for testing (and avoid ARP flux / `rp_filter` drops that come from having two NICs on the same L2 segment in one namespace), each device is moved into its own network namespace.

`scripts/setup_netns.sh` automates the steps below and is safe to re-run:

```bash
sudo bash scripts/setup_netns.sh
```

What it does, and why each step is there:

```bash
# 1. Discover the device and netdev names. Do not hardcode them: rdma-core's
#    udev rules may or may not rename a device between boots, so the same card
#    can be mlx5_0 one day and rocep3s0 the next. Everything below reads them.
rdma link
# link rocep3s0/1 state ACTIVE physical_state LINK_UP netdev ens2np0
# link rocep4s0/1 state ACTIVE physical_state LINK_UP netdev ens4np0
DEV1=rocep3s0 NETDEV1=ens2np0
DEV2=rocep4s0 NETDEV2=ens4np0

# 2. Find the RoCEv2 GID. A port carries several: one per address on its
#    netdev, plus an auto-generated IPv6 link-local pair. Taking the first
#    would land on link-local, while rdma_cm resolves an IPv4 peer to the
#    IPv4-mapped entry — the two modes would not even use the same IP version.
for f in /sys/class/infiniband/$DEV1/ports/1/gids/*; do
  gid=$(cat "$f")
  [ "$gid" != "0000:0000:0000:0000:0000:0000:0000:0000" ] && \
    echo "idx=$(basename "$f") gid=$gid type=$(cat "${f/gids/gid_attrs\/types}")"
done
# idx=1  fe80:...            RoCE v2   <- link-local, not what we want
# idx=3  ...ffff:c0a8:6401   RoCE v2   <- the IPv4 address, this one

# 3. Switch to netns-exclusive mode. Do this before anything binds the
#    devices; it does not survive a reboot.
sudo rdma system set netns exclusive

# 4. Create two namespaces and hand one device to each. The netdev does not
#    always follow its RDMA device, so move it explicitly.
sudo ip netns add ns1
sudo ip netns add ns2
sudo rdma dev set $DEV1 netns ns1 && sudo ip link set $NETDEV1 netns ns1
sudo rdma dev set $DEV2 netns ns2 && sudo ip link set $NETDEV2 netns ns2

# 5. Verify: one device in each namespace, none left in the root
sudo ip netns exec ns1 rdma dev show
sudo ip netns exec ns2 rdma dev show
sudo rdma dev show                     # (empty)

# 6. Address them. Moving an interface between namespaces resets it to down
#    and flushes its addresses, so this has to happen after the move.
sudo ip netns exec ns1 ip addr add 192.168.100.1/24 dev $NETDEV1
sudo ip netns exec ns1 ip link set $NETDEV1 up
sudo ip netns exec ns2 ip addr add 192.168.100.2/24 dev $NETDEV2
sudo ip netns exec ns2 ip link set $NETDEV2 up

# 7. 100GbE autonegotiation takes seconds — wait for ACTIVE before testing
sudo ip netns exec ns1 rdma link
sudo ip netns exec ns1 ping -c2 192.168.100.2
```

With this in place, `ns1` and `ns2` behave like two separate machines connected over RoCEv2 — benchmarks run with `sudo ip netns exec ns1 <binary> ...` / `sudo ip netns exec ns2 <binary> ... 192.168.100.1`.

## What the numbers were measured on

| | |
|---|---|
| NICs | 2× Mellanox ConnectX-4 (MT27700), 100GbE, one per PCIe slot |
| Link | QSFP28 DAC, port to port, no switch |
| Path MTU | 1024 B — the largest IB MTU that fits the netdev's 1500 B Ethernet MTU |
| CPU | Xeon E5-1650 v4, 6 cores / 12 threads, single NUMA node |
| OS | Ubuntu 22.04, kernel 5.15, rdma-core from the distro |

Three things are set before a sweep and restored after (`scripts/run_phase1_sweep.sh` does both):

- **CPU governor pinned to `performance`.** Small messages are message-rate bound, so the cost is the CPU posting work requests — and a scaling governor parks a busy-waiting core at its minimum. Leaving `schedutil` on inflated run-to-run spread from 0.5% to 2.2% and produced one 36% outlier.
- **irqbalance stopped**, so NIC interrupts do not migrate mid-run.
- **Server and client pinned to different physical cores.** Both busy-spin; landing them on two hyperthreads of one core has them fighting over the same execution units.

**Both processes are on one machine.** The RDMA path is real — two cards, a real cable, real DMA — but CPU, memory bandwidth and PCIe are shared, which a two-machine setup would not do. It is the right caveat to keep in mind for the bandwidth numbers in particular.

