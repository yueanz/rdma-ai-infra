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

> All Phase 1 benchmarks measured on two Alibaba Cloud ECS machines using the same rdma_cm codebase — once with eRDMA (hardware RDMA) and once with SoftRoCE (software RDMA over UDP, `rdma_rxe`). Same machines, same code, different RDMA device.

### Phase 1 — eRDMA (Alibaba Cloud ECS, two machines)

| Benchmark | Min | Median | p99 | Max |
|---|---|---|---|---|
| `lat_send_recv` (RTT) | 38.30 μs | 39.93 μs | 44.96 μs | 52.59 μs |
| `lat_rdma_write` (one-sided) | 30.32 μs | 31.46 μs | 36.24 μs | 42.41 μs |

| Benchmark | Config | Throughput |
|---|---|---|
| `bw_rdma_write` | 1MB × 100 iters, depth=1 (burst) | 9.06 GB/s / 77.83 Gbps |
| `bw_rdma_write` | 1MB × 1000 iters, depth=1 (sustained) | 3.30 GB/s / 28.37 Gbps |

64KB depth sweep (1000 iters):

| depth | eRDMA | SoftRoCE |
|---|---|---|
| 1 | 1.63 GB/s / 13.98 Gbps | 0.90 GB/s / 7.71 Gbps |
| 2 | 3.01 GB/s / 25.82 Gbps | 1.27 GB/s / 10.91 Gbps |
| 4 | 4.71 GB/s / 40.44 Gbps | 1.65 GB/s / 14.17 Gbps |
| 8 | 5.74 GB/s / 49.33 Gbps | — (UDP buffer overflow) |
| 16 | 6.72 GB/s / 57.76 Gbps | — |
| 32 | WR_FLUSH_ERR | — |

**Key insights:**
- RDMA write is ~21% lower latency than send/recv (median: 31 vs 40 μs) — one-sided ops bypass server-side CPU entirely
- Burst vs sustained throughput gap (78 → 28 Gbps) reflects Alibaba Cloud eRDMA fabric QoS: short transfers run at line rate, sustained transfers are throttled to a committed rate
- eRDMA throughput scales near-linearly to depth=8, then diminishing returns as depth=16 saturates the NIC (~58 Gbps); depth=32 hits eRDMA fabric's WR limit
- SoftRoCE caps out at depth=4 (14 Gbps); depth≥8 triggers UDP buffer overflow — 65KB × 8 in-flight = 128 concurrent UDP packets exceeds the kernel receive buffer; eRDMA at depth=4 (40 Gbps) already outperforms SoftRoCE's ceiling

### Phase 1 — SoftRoCE (Alibaba Cloud ECS, two machines)

Same physical machines as above; eRDMA unloaded (`modprobe -r erdma`), SoftRoCE loaded on the same NIC (`rdma link add rxe0 type rxe netdev eth0`). Differences reflect software vs hardware RDMA only.

| Benchmark | Min | Median | p99 | Max |
|---|---|---|---|---|
| `lat_send_recv` (RTT) | 41.21 μs | 46.72 μs | 53.49 μs | 61.97 μs |
| `lat_rdma_write` (one-sided) | 39.31 μs | 40.06 μs | 43.25 μs | 50.69 μs |

**Key insights:**
- eRDMA is ~22% lower latency than SoftRoCE (RDMA write median: 31 vs 40 μs) on the same hardware
- 1MB writes fail on SoftRoCE (`transport retry counter exceeded`) due to UDP fragmentation; 64KB is the practical ceiling for rdma_rxe
- See depth sweep table in the eRDMA section above for full throughput comparison

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
│   │   └── rdma_common.h            # rai_ctx_t, rai_qp_t, rai_mr_t
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
