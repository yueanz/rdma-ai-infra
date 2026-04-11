# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` (no wrappers, no frameworks), progressing from raw verbs to a mini NCCL-style all-reduce.

## Phase Status

- [x] **Phase 1** — RDMA Verbs Foundation (RC QP, MR, CQ, send/recv, RDMA write, benchmarks)
- [x] **Phase 2** — Transport Abstraction Layer (RDMA + TCP backends, send/recv + write benchmarks)
- [x] **Phase 3** — Ring All-Reduce (chunked pipeline, ring reduce-scatter + all-gather, TCP backend)
- [ ] **Phase 4b** — Remote KV Cache (RDMA-based, prefill/decode access pattern)

## Benchmark Results

> Measured on SoftRoCE (rdma_rxe) over loopback. SoftRoCE runs in-kernel over UDP — latency is not representative of real RDMA hardware (ConnectX NICs show ~1–3 μs). Numbers here validate correctness and relative ordering only.

### Phase 1 — Raw Verbs (DigitalOcean 2GB VM, Ubuntu 22.04)

| Benchmark | Min | Median | p99 |
|---|---|---|---|
| `lat_send_recv` (RTT) | 1265 μs | 2000 μs | 4952 μs |
| `lat_rdma_write` (one-sided) | 4 μs | 6 μs | 45 μs |
| `bw_rdma_write` (throughput) | — | 1.5 Gbps | — |

**Key insight:** RDMA write is 300x lower latency than send/recv on SoftRoCE because it bypasses the kernel receive path. On real hardware the gap is even larger (~10x).

### Phase 2 & 3 — Planned: Real RoCE Hardware (OCI BM.Optimized3.36)

Benchmarks for Phase 2 (RDMA vs TCP backend) and Phase 3 (ring all-reduce RDMA) will be measured on two OCI BM.Optimized3.36 bare-metal instances connected via OCI RDMA cluster network (RoCE v2, Mellanox ConnectX-6). Expected results:

| Benchmark | Expected |
|---|---|
| `lat_rdma_write` | ~1–3 μs |
| `lat_send_recv` RDMA | ~2–5 μs |
| `bw_rdma_write` | ~10–25 Gbps |
| ring all-reduce RDMA, N=4, 1GB | TBD |

> SoftRoCE (software RDMA over UDP) does not activate kernel-bypass, so RDMA vs TCP comparisons are only meaningful on real hardware.

### Phase 3 — Ring All-Reduce (Azure VM, TCP backend, loopback)

| Benchmark | Min | Median | p99 |
|---|---|---|---|
| `ring_allreduce` TCP, N=2, 1024 floats (4KB) | 45 μs | 95 μs | 121 μs |

> TCP loopback baseline. RDMA backend benchmark pending real RoCE hardware.

## Architecture

```
rdma-ai-infra/
│
├── common/                          # linked by all phases
│   └── include/
│       ├── timing.h                 # CLOCK_MONOTONIC nanosecond timer
│       ├── logging.h                # LOG_INFO / LOG_ERR macros
│       └── bench_utils.h            # print_latency / print_bandwidth
│
├── phase1_verbs/                    # Pure C ────────────────────────────
│   ├── include/
│   │   └── rdma_common.h            # rdma_ctx_t, rdma_qp_t, rdma_mr_t
│   ├── src/
│   │   ├── rdma_context.c           # device open, PD, CQ lifecycle
│   │   ├── rdma_qp.c                # QP create + state machine
│   │   ├── rdma_mr.c                # MR register / deregister
│   │   ├── rdma_ops.c               # post_send / post_recv / post_write / poll_cq
│   │   └── rdma_connect.c           # OOB TCP handshake
│   └── bench/
│       ├── lat_send_recv.c          # two-sided ping-pong latency
│       ├── lat_rdma_write.c         # one-sided write latency
│       └── bw_rdma_write.c          # throughput (sliding window + unsignaled WRs)
│                                    # ── C / C++ boundary ────────────────
│
├── phase2_transport/                # C++17
│   ├── include/
│   │   ├── transport.hpp            # Transport pure virtual base class + ScopedBuffer
│   │   ├── rdma_backend.hpp
│   │   └── tcp_backend.hpp
│   ├── src/
│   │   ├── rdma_backend.cpp         # wraps phase1 via extern "C"
│   │   └── tcp_backend.cpp
│   └── bench/
│       └── backend_compare.cpp      # send/recv + write latency over both backends
│
├── phase3_collective/               # C++17
│   ├── include/
│   │   └── collective.hpp           # World struct, world_init, ring_allreduce
│   ├── src/
│   │   ├── world.cpp                # process group init, parallel listen/connect
│   │   └── ring_allreduce.cpp       # chunked ring all-reduce (float32 sum)
│   └── bench/
│       └── allreduce_bench.cpp      # correctness check + latency benchmark
│
├── phase4b_kv_cache/                # C++ — planned
│
├── scripts/
│   └── setup.sh                     # apt install + SoftRoCE setup + build
│
├── CMakeLists.txt
└── README.md
```

## Build

```bash
# On Ubuntu 22.04
sudo apt install build-essential cmake libibverbs-dev ibverbs-utils rdma-core
sudo apt install linux-modules-extra-$(uname -r)

# Enable SoftRoCE
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0

# Build
mkdir build && cd build
cmake .. && make
```

## Running Benchmarks

```bash
# Phase 1
cd build/phase1_verbs

# Terminal 1 — server
./lat_send_recv

# Terminal 2 — client
./lat_send_recv 127.0.0.1

# RDMA write latency
./lat_rdma_write
./lat_rdma_write 127.0.0.1

# Throughput
./bw_rdma_write --size 65536 --iters 10000 --depth 32
./bw_rdma_write 127.0.0.1 --size 65536 --iters 10000 --depth 32

# Phase 2
cd build/phase2_transport

# Terminal 1 — server
./backend_compare <rdma|tcp> <port>

# Terminal 2 — client
./backend_compare <rdma|tcp> <server_ip> <port>
```

## Environment

- OS: Ubuntu 22.04 LTS
- RDMA: SoftRoCE (`rdma_rxe`) over loopback for development; designed for real InfiniBand / RoCE hardware
- Compiler: GCC 11+, `-std=c11` (Phase 1), `-std=c++17` (Phase 2+)
