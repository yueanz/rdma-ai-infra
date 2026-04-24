# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` and `rdma_cm` (no wrappers, no frameworks), progressing from raw verbs to a mini NCCL-style all-reduce.

## Phase Status

- [x] **Phase 1** — RDMA Verbs Foundation (RC QP, MR, CQ, send/recv, RDMA write, benchmarks; rdma_cm connection for iWARP/RoCE portability)
- [x] **Phase 2** — Transport Abstraction Layer (RDMA + TCP backends, send/recv + write benchmarks)
- [x] **Phase 3** — Ring All-Reduce (chunked pipeline, ring reduce-scatter + all-gather, TCP backend)
- [x] **Phase 4b** — Remote KV Cache (slab allocator over single MR, ctrl/data plane separation, prefill via RDMA write, decode via RDMA read)
- [ ] **Phase 5** — vLLM KVTransferAgent Integration (pybind11 binding for Transport layer, `RdmaKVConnector` implementing `KVConnectorBase_V1`, CPU tensor path validated end-to-end, GPUDirect designed for future hardware)

## Benchmark Results

> Measured on SoftRoCE (rdma_rxe) over loopback. SoftRoCE runs in-kernel over UDP — latency is not representative of real RDMA hardware (ConnectX NICs show ~1–3 μs). Numbers here validate correctness and relative ordering only.

### Phase 1 — Raw Verbs (DigitalOcean 2GB VM, Ubuntu 22.04, SoftRoCE)

| Benchmark | Min | Median | p99 |
|---|---|---|---|
| `lat_send_recv` (RTT) | 1265 μs | 2000 μs | 4952 μs |
| `lat_rdma_write` (one-sided) | 4 μs | 6 μs | 45 μs |
| `bw_rdma_write` (throughput) | — | 1.5 Gbps | — |

**Key insight:** RDMA write is 300x lower latency than send/recv on SoftRoCE because it bypasses the kernel receive path. On real hardware the gap is even larger (~10x).

### Phase 1 — rdma_cm (Alibaba Cloud ECS, eRDMA, two machines)

| Benchmark | Min | Median | p99 | Max |
|---|---|---|---|---|
| `lat_send_recv` (RTT) | 37.76 μs | 39.93 μs | 42833 μs | 45483 μs |
| `lat_rdma_write` (one-sided) | 30.28 μs | 31.58 μs | 37.22 μs | 51.51 μs |

| Benchmark | Config | Throughput |
|---|---|---|
| `bw_rdma_write` | 1MB × 100 iters, depth=1 (burst) | 9.11 GB/s / 78 Gbps |
| `bw_rdma_write` | 1MB × 1000 iters, depth=1 (sustained) | 3.30 GB/s / 28 Gbps |

**Key insights:**
- RDMA write is ~20% lower latency than send/recv (one-sided: no server-side CPU involvement)
- RDMA write p99 (37 μs) vs send/recv p99 (43 ms) — one-sided ops are immune to OS scheduling jitter on the server
- Burst vs sustained throughput gap (78 → 28 Gbps) reflects Alibaba Cloud eRDMA fabric QoS shaping: short transfers run at line rate, sustained transfers are throttled to a committed rate
- eRDMA depth limit: for 1MB messages, depth > 1 (unsignaled WRs) triggers WR_FLUSH_ERR; depth=1 is required for reliable large-message benchmarking

> Connection established via rdma_cm, which provides portability across eRDMA/iWARP, RoCE, and InfiniBand without code changes. p99 spike on send/recv reflects OS scheduling jitter on shared cloud VMs; RDMA write p99 is clean because only one local CQ poll is needed.

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

### Phase 4b — Remote KV Cache (slot_size=4096B)

| Benchmark | Environment | Min | Median | p99 | Max |
|---|---|---|---|---|---|
| `kv_bench` prefill (RDMA write) | Azure MANA RoCE, loopback | 8.41 μs | 8.50 μs | 10.28 μs | 59.89 μs |
| `kv_bench` decode (RDMA read) | SoftRoCE, loopback | 8.80 μs | 8.90 μs | 9.20 μs | 25.40 μs |

Throughput (1000 iters × 4096B): prefill **0.44 GB/s / 3.79 Gbps** · decode **0.42 GB/s / 3.63 Gbps**

> Prefill measured on Azure MANA RoCE hardware. Decode measured on SoftRoCE (rdma_rxe) over loopback — latency not representative of real hardware.

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
├── phase4b_kv_cache/                # C++17
│   ├── include/
│   │   └── kv_cache.hpp             # KVPool (slab allocator), KVRemote, KVMeta, CtrlBuf
│   ├── src/
│   │   └── kv_server.cpp            # ctrl (TCP) + data (RDMA) server, ALLOC/FREE protocol
│   └── bench/
│       └── kv_bench.cpp             # prefill (RDMA write) + decode (RDMA read) benchmark
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
# Phase 4b — Remote KV Cache
cd build/phase4b_kv_cache

# Terminal 1 — server (port, num_slots, slot_size)
./kv_server 12345 16 4096

# Terminal 2 — client
./kv_bench <server_ip> 12345 [--iters <n>]
```

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

# Throughput (server and client must use the same --size)
./bw_rdma_write --size 1048576
./bw_rdma_write <server_ip> --size 1048576 --iters 1000 --depth 1

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
