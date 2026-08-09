# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` and `rdma_cm` (no wrappers, no frameworks). On top of
the verbs layer sit two end-to-end workloads: a ring all-reduce (chunked
reduce-scatter then all-gather, the algorithm NCCL uses), and a remote memory
pool that clients fill by RDMA write and read back one-sidedly — the transfer
path a disaggregated KV cache runs on.

Tested on a real RDMA home lab — two ConnectX-4 NICs, RoCEv2, bare metal,
back-to-back, each port in its own network namespace so it behaves as an
independent host — and measured against perftest: latency within 4% at every
message size, bandwidth within 2% wherever the link is the bottleneck.

## Phase Status

- [x] **Phase 1** — RDMA verbs foundation: RC queue pairs, memory regions,
  completion queues, send/recv, write, read, inline send. Connections come up
  either through librdmacm or through a hand-written INIT → RTR → RTS state
  machine, selectable at runtime; the data path is shared.
- [x] **Phase 2** — Transport Abstraction Layer (RDMA + TCP backends via rdma_cm, send/recv + write benchmarks; TCP write omitted — no one-sided primitive)
- [x] **Phase 3** — Ring All-Reduce (chunked pipeline, ring reduce-scatter + all-gather, RDMA + TCP backends)
- [x] **Phase 4** — Remote slot pool (slab allocator over a single MR; alloc/free on a control channel, data by one-sided write and read, so the server's CPU is not in the data path)
- [ ] **Phase 5** — Bare-metal RoCEv2 benchmark (in progress). Re-running the Phase 1–4 benchmarks on real ConnectX-4 hardware over RoCEv2.

## Hardware & Test Environment

### Hardware

| | |
|---|---|
| ![Chassis before install](pictures/01-chassis-before-install.jpg) | Chassis opened, RAM in, PCIe slots still empty. |
| ![ConnectX-4 NICs unboxed](pictures/02-connectx4-nics-unboxed.jpg) | Two Mellanox ConnectX-4 100GbE NICs. |
| ![ConnectX-4 installed](pictures/03-connectx4-installed.jpg) | Both cards seated, wired to each other with a QSFP28 DAC — no switch in the loop. |

### Bring-up

The two ports are wired straight to each other. Each device then goes into its
own network namespace, so one host behaves like two independent peers — and so
two NICs on one L2 segment cannot confuse each other's ARP.

```bash
sudo bash scripts/setup_netns.sh     # idempotent; nothing here survives a reboot
```

| | |
|---|---|
| NICs | 2× Mellanox ConnectX-4 (MT27700), 100GbE |
| Link | QSFP28 DAC, port to port, no switch |
| Path MTU | 1024 B, from the netdev's 1500 B Ethernet MTU |
| CPU | Xeon E5-1650 v4, 6C/12T, single NUMA node |
| OS | Ubuntu 22.04, kernel 5.15 |

Sweeps pin the CPU governor to `performance`, stop irqbalance, and put server
and client on different physical cores. **Both processes are on one machine**:
the RDMA path is real, but CPU, memory bandwidth and PCIe are shared in a way
two machines would not be.

Step-by-step, and what each step is guarding against:
[`docs/hardware-setup.md`](docs/hardware-setup.md).

## Architecture

```
common/            timing, logging, benchmark stats; the CLI and config the
                   Phase 1 benchmarks share
phase1_verbs/      C. RC queue pairs, memory regions, send/recv/write/read, poll.
                   Two ways to bring a connection up — librdmacm, or the
                   INIT→RTR→RTS state machine by hand — behind one call shape.
phase2_transport/  C++17. One Transport interface over RDMA and TCP backends.
phase3_collective/ C++17. Ring all-reduce: chunked reduce-scatter + all-gather.
phase4_kv_cache/   C++17. Slab allocator over one MR. Alloc/free control plane,
                   one-sided data plane; the server is a passive target.
python_bindings/   pybind11 wrapper around Transport. Registers any buffer-protocol
                   object (numpy, torch) for RDMA without copying it.
scripts/           bring-up, benchmark sweep, plotting.
```

## Build

```bash
bash scripts/setup.sh                      # dependencies, then build
cmake -B build && cmake --build build -j   # just the build, deps already in place
```

Defaults to `RelWithDebInfo`; pass `-DCMAKE_BUILD_TYPE=Debug` for gdb work.

## Running Benchmarks

The whole sweep, against perftest at the same points, plus the figures:

```bash
sudo bash scripts/run_phase1_sweep.sh   # ~10 min -> results/phase1_sweep.csv
python3 scripts/plot_phase1.py          # -> results/*.png
```

One benchmark on its own — server in `ns1`, client in `ns2`:

```bash
sudo ip netns exec ns1 ./build/phase1_verbs/lat_send_recv
sudo ip netns exec ns2 ./build/phase1_verbs/lat_send_recv 192.168.100.1
```

Same shape for the others; `--help` lists the options. Both sides must agree on
`--size`, `--depth` and `--conn`.

`--conn` picks who sets the connection up: `cm` hands it to librdmacm, `verbs`
walks the queue pair through INIT → RTR → RTS in this code. After that both run
the same send and receive path.

## Benchmark Results

Phase 1 only. Phases 2–4 have not been re-run on this hardware yet — that is
what Phase 5 is.

Median of at least five runs per point, with the equivalent perftest tool run
at the same points on the same link. Latency is one-way throughout: these
benchmarks time a round trip, perftest halves it before printing.

- **Latency within 4% of perftest at every size**, bandwidth within 2%
  wherever the link is the bottleneck.
- **Receive queue depth decides how often a slow path gets hit** — one work
  request posted instead of eight costs 1.78 → 3.56 µs one-way. perftest moves
  the same way, 1.83 → 3.66: this is the hardware, not the implementation.
- **One-sided is not the faster one** — 8% at 64 B here, 9% for perftest, and
  nothing at all by 1 MB. What it buys is that the far side posts no work
  requests and reaps no completions: CPU, not µs.

### Receive queue depth

![latency vs receive queue depth](results/phase1-latency-vs-rq-depth.png)

An arriving message needs a receive work queue entry before the NIC can place
it. With one posted the NIC often has to go fetch it while the packet is already
arriving; with eight it almost never does. Past eight the curve is flat.

Nothing gets slower — you just hit the slow case more often. At depth 8 the p99
is 3.60 µs — about what the median is at depth 1. One slow path, two
frequencies. It is also why depth 1 is the only point whose median wobbles from
run to run.

perftest's `-r` rounds odd values up, so its line starts at 2.

The one-sided line posts no receive work requests at all, so this knob cannot
reach it, and its flatness is what says the drop beside it is a real effect
rather than drift during the sweep.

### Latency and bandwidth against perftest

![latency vs message size](results/phase1-latency-vs-size.png)

One-way latency in µs, with perftest's figure for the same operation in
parentheses:

| | 64 B | 4 KB | 64 KB | 1 MB |
|---|---|---|---|---|
| send/recv | 0.88 (0.86) | 1.77 (1.83) | 8.54 (8.70) | 94.5 (95.1) |
| write | 0.81 (0.79) | 1.76 (1.80) | 8.39 (8.59) | 94.6 (95.0) |

Messages of 220 B or less go inline: the payload rides inside the work request
itself, sparing the NIC a separate fetch of the buffer. Before that was added
the 64 B row read 1.14 and 1.11 — 33–40% behind.

Write bandwidth reaches 92.4 Gbps at 16 KB, 92.0 at 64 KB and 90.8 at 1 MB
(`ib_write_bw`: 92.5, 92.5, 92.6). Below 16 KB neither tool is link-bound and
the result turns on how each one pipelines; they are not configured
equivalently there, so that range is left out rather than claimed.

### Connection setup mode

librdmacm and the hand-written INIT → RTR → RTS path are indistinguishable —
1.77 vs 1.77 µs send/recv, 92.03 vs 92.05 Gbps — which is the expected answer,
since setup happens once, before the loop being timed. The result worth having
is that the state machine is a drop-in for the library, not that it is quicker.

---

Several of these numbers were wrong first, including three separate occasions
where this implementation appeared to beat perftest and every one turned out to
be an error in the measurement. The trail is in
[`docs/benchmark-notes.md`](docs/benchmark-notes.md).

## Toolchain

- **OS**: Ubuntu 22.04 LTS
- **Compiler**: GCC 11.4, `-std=c11` (Phase 1), `-std=c++17` (Phase 2+)
- **Build**: CMake 3.16+
