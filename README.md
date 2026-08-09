# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` and `rdma_cm` (no wrappers, no frameworks), implementing two end-to-end workloads from raw verbs upward: a mini NCCL-style ring all-reduce and a vLLM-style remote KV cache (prefill via RDMA write, decode via RDMA read).

Tested on a real RDMA home lab: two ConnectX-4 NICs, RoCEv2, bare metal, connected back-to-back and isolated into separate network namespaces so each port behaves like an independent host.

## Phase Status

- [x] **Phase 1** — RDMA verbs foundation: RC queue pairs, memory regions, completion queues, send/recv, write, read. Connections come up either through librdmacm or through the INIT → RTR → RTS state machine written out by hand, selectable at runtime; the data path is shared. An earlier round used raw verbs only and had to abandon it — [`docs/raw-verbs-evolution.md`](docs/raw-verbs-evolution.md) is why, and why the hand-written path came back.
- [x] **Phase 2** — Transport Abstraction Layer (RDMA + TCP backends via rdma_cm, send/recv + write benchmarks; TCP write omitted — no one-sided primitive)
- [x] **Phase 3** — Ring All-Reduce (chunked pipeline, ring reduce-scatter + all-gather, RDMA + TCP backends)
- [x] **Phase 4** — Remote KV Cache (slab allocator over single MR, ctrl/data plane separation, prefill via RDMA write, decode via RDMA read)
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
common/            timing, logging, benchmark stats and CLI shared by every phase
phase1_verbs/      C. RC queue pairs, memory regions, send/recv/write/read, poll.
                   Two ways to bring a connection up — librdmacm, or the
                   INIT→RTR→RTS state machine by hand — behind one call shape.
phase2_transport/  C++17. One Transport interface over RDMA and TCP backends.
phase3_collective/ C++17. Ring all-reduce: chunked reduce-scatter + all-gather.
phase4_kv_cache/   C++17. Slab-allocated remote KV store; prefill writes, decode reads.
python_bindings/   pybind11 wrapper around Transport, torch.Tensor aware.
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

Median of five runs per point, with the equivalent perftest tool run at the
same points on the same link. Latency is one-way throughout: these benchmarks
report a round trip, perftest halves it before printing, so the two are
normalised to match.

- **Latency within 4% of perftest from 256 B up**, bandwidth within 2%
  wherever the link is the bottleneck.
- **Receive queue depth doubles two-sided latency** — one work request posted
  instead of eight costs 1.78 → 3.55 µs one-way. perftest degrades by the same
  amount, so this is the hardware, not the implementation.
- **One-sided is not the faster one** — 0–8%, and none of it at 1 MB. What it
  buys is that the far side posts nothing and reaps nothing: CPU, not µs.

### Receive queue depth

![latency vs receive queue depth](results/phase1-latency-vs-rq-depth.png)

An arriving message needs a receive work queue entry before the NIC can place
it. Keep only one posted and the NIC has to go fetch it, on the critical path,
every time. Eight is enough; past that the curve is flat.

The one-sided line is there as a check on the machine rather than a comparison
— it posts no receive work requests, so this knob cannot reach it, and its
flatness is what says the drop beside it is a real effect and not drift during
the sweep.

### Latency and bandwidth against perftest

![latency vs message size](results/phase1-latency-vs-size.png)

One-way latency in µs, with perftest's figure for the same operation in
parentheses:

| | 64 B | 4 KB | 64 KB | 1 MB |
|---|---|---|---|---|
| send/recv | 1.14 (0.86) | 1.77 (1.80) | 8.45 (8.68) | 94.4 (94.6) |
| write | 1.11 (0.79) | 1.76 (1.80) | 8.38 (8.56) | 94.5 (94.5) |

64 B is the one place they part: perftest sends a message that small inline,
riding inside the work request and sparing the NIC a fetch from host memory.
That is not implemented here and it costs 33–40%.

Write bandwidth reaches 92.4 Gbps at 16 KB, 91.9 at 64 KB and 90.7 at 1 MB
(`ib_write_bw`: 92.5, 92.5, 92.6). Below 16 KB neither tool is link-bound and
the result turns on how each one pipelines; they are not configured
equivalently there, so that range is left out rather than claimed.

### Connection setup mode

librdmacm and the hand-written INIT → RTR → RTS path are indistinguishable —
3.54 vs 3.54 µs send/recv, 92.04 vs 92.03 Gbps — which is the expected answer,
since setup happens once, before the loop being timed. The result worth having
is that the state machine is a drop-in for the library, not that it is quicker.

---

Several of these numbers were wrong first, including three separate occasions
where this implementation appeared to beat perftest and every one turned out to
be an error in the measurement. The trail is in
[`docs/benchmark-notes.md`](docs/benchmark-notes.md).

## Toolchain

- **OS**: Ubuntu 22.04 LTS
- **Compiler**: GCC 7+, `-std=c11` (Phase 1), `-std=c++17` (Phase 2+)
