# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` and `rdma_cm` (no wrappers, no frameworks), implementing two end-to-end workloads from raw verbs upward: a mini NCCL-style ring all-reduce and a vLLM-style remote KV cache (prefill via RDMA write, decode via RDMA read).

Tested on a real RDMA home lab: two ConnectX-4 NICs, RoCEv2, bare metal, connected back-to-back and isolated into separate network namespaces so each port behaves like an independent host.

## Phase Status

- [x] **Phase 1** — RDMA Verbs Foundation (RC QP, MR, CQ, send/recv, RDMA write, benchmarks). Uses `rdma_cm` for connection setup — some RDMA fabrics (e.g. iWARP) reject manual `ibv_modify_qp` state transitions, so this project standardizes on `rdma_cm` across all backends. Migration retrospective: [`docs/raw-verbs-evolution.md`](docs/raw-verbs-evolution.md).
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
`--size`, `--depth` and `--conn`. `--conn cm|verbs` selects how the queue pair
is brought up — librdmacm, or the state machine by hand — over an identical
data path, so a matched pair of runs isolates connection setup and nothing else.

## Benchmark Results

_Pending — numbers go here as they're collected._

## Toolchain

- **OS**: Ubuntu 22.04 LTS
- **Compiler**: GCC 7+, `-std=c11` (Phase 1), `-std=c++17` (Phase 2+)
