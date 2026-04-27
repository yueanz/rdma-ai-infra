# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` and `rdma_cm` (no wrappers, no frameworks), progressing from raw verbs to a mini NCCL-style all-reduce.

## Phase Status

- [x] **Phase 1** — RDMA Verbs Foundation (RC QP, MR, CQ, send/recv, RDMA write, benchmarks; rdma_cm connection for iWARP/RoCE portability)
- [x] **Phase 2** — Transport Abstraction Layer (RDMA + TCP backends via rdma_cm, send/recv + write benchmarks; TCP write omitted — no one-sided primitive)
- [x] **Phase 3** — Ring All-Reduce (chunked pipeline, ring reduce-scatter + all-gather, TCP backend)
- [x] **Phase 4** — Remote KV Cache (slab allocator over single MR, ctrl/data plane separation, prefill via RDMA write, decode via RDMA read)
- [ ] **Phase 5** — vLLM KVTransferAgent Integration (pybind11 binding for Transport layer, `RdmaKVConnector` implementing `KVConnectorBase_V1`, CPU tensor path validated end-to-end, GPUDirect designed for future hardware)

## Planned Refactors

- [x] **Drop the raw verbs path, keep only rdma_cm**
  - **Why**: production target is Alibaba Cloud eRDMA (iWARP), which rejects manual `ibv_modify_qp INIT/RTR/RTS` transitions and requires rdma_cm. The raw verbs path was an early-stage learning artifact, not used in any current benchmark.
  - **Done**: Deleted `rai_ctx_init / rai_qp_create / rai_qp_init / rai_qp_connect` (~200 LOC removed); simplified `rai_ctx_t` (dropped `port` / `gid_index` fields). The OOB TCP helpers (`rai_oob_listen / accept / connect`) remain because they're now used by `rai_cm_listen_qp` to set up the MR-exchange channel on `port+1`.
- [ ] **Write `docs/raw-verbs-evolution.md`** retrospective covering the manual QP state machine, OOB TCP handshake, bugs we hit (PSN sync, GID selection, RNR retry on SoftRoCE, eRDMA rejecting manual transitions), and why we moved to rdma_cm. Tag a commit on the pre-cleanup snapshot for reference.
- [ ] **Re-measure Phase 3 on Alibaba eRDMA** — current Phase 3 number is TCP loopback on Azure VM (historical). Run `allreduce_bench` with both TCP and RDMA backends on the eRDMA two-machine setup.
- [ ] **Re-measure Phase 4 on Alibaba eRDMA** — current Phase 4 numbers are from Azure MANA RoCE (prefill) and SoftRoCE loopback (decode), historical before the eRDMA commitment.
- [x] **Fold `rai_ctx_t` into `rai_qp_t` and delete `rdma_context.c`** — Done. Moved PD/CQ into `rai_qp_t`, deleted `rai_ctx_t` and `rdma_context.c`, simplified all APIs from `(ctx, qp)` to `(qp)` (`rai_mr_reg`, `rai_poll_cq`, `rai_cm_server/client/listen_qp/connect_qp`). Updated ~70 call sites across phase1 verbs/benchmarks and phase2 backend.

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

### Phase 2 — backend_compare (Alibaba Cloud ECS, eRDMA, two machines)

Compares RDMA backend (rdma_cm + libibverbs) against TCP backend across send/recv and write semantics, at multiple message sizes.

> **Note**: Phase 2's RDMA backend originally used raw verbs OOB handshake. eRDMA (iWARP-based) rejects manual `ibv_modify_qp` state transitions, so the backend was migrated to rdma_cm — see [Planned Refactors](#planned-refactors). The migrated path is what's measured below.

**send/recv latency (RTT, two-sided echo)**

| Backend | Size | Min | Median | p99 | Max | Throughput |
|---|---|---|---|---|---|---|
| RDMA | 4KB | 23.42 | 25.10 | **87,806** | 90,814 | 0.01 Gbps |
| RDMA | 64KB | 31.91 | 34.17 | 87.56† | 45,655 | 2.02 Gbps |
| RDMA | 1MB | 119.78 | 181.25 | 617.90 | 45,550 | 23.34 Gbps |
| TCP | 4KB | 28.70 | 30.26 | 37.89 | 45.61 | 1.07 Gbps |
| TCP | 64KB | 84.75 | 105.99 | 115.66 | 127.46 | 4.93 Gbps |
| TCP | 1MB | 185.34 | 198.42 | 286.02 | 4,211 | **37.84 Gbps** |

† RDMA send/recv p99 is **bimodal**: clean (~87 µs) when no RNR fires; spikes to tens of ms (max column) when cloud-VM scheduling delays the server's `post_recv` past the client's send (RNR retry × `min_rnr_timer 31` = ~3.4 sec each). Run-to-run variance is huge — same code, same environment.

**RDMA write latency (one-sided, no server CPU involvement)**

| Size | Min | Median | p99 | Max | Throughput |
|---|---|---|---|---|---|
| 4KB | 16.87 | 18.11 | 22.16 | 25.35 | 1.82 Gbps |
| 64KB | 21.32 | 23.29 | 31.13 | 64.20 | **22.36 Gbps** |
| 1MB | 119.96 | 321.37 | 426.59 | 1,827 | 25.85 Gbps |

> TCP write benchmark is **omitted**: TCP has no one-sided write primitive. Any TCP "write" either measures local syscall time (misleading — completion ≠ remote received) or degenerates into a 2-sided send/recv with explicit ACK (duplicating the send/recv numbers). RDMA write's NIC-level ACK is a protocol-level capability software cannot replicate.

**Key insights:**

- **At 4KB (small messages)**: RDMA write median 18 µs vs TCP send/recv 30 µs — **RDMA wins on latency** by 40%, and write is even faster than RDMA send/recv (one-sided skips server CPU).
- **At 64KB (medium)**: RDMA write 22 Gbps vs RDMA send/recv 2 Gbps — **single-byte difference is 11×**. The send/recv throughput collapses because the RNR retry tail amplifies cloud-VM scheduling jitter into milliseconds. RDMA write is immune (server CPU not involved).
- **At 1MB (large)**: TCP send/recv (37.84 Gbps) **outperforms** all RDMA modes (~25 Gbps). Reason: Alibaba eRDMA fabric applies QoS shaping to RDMA traffic at ~25–28 Gbps sustained (matches Phase 1 `bw_rdma_write` 28 Gbps sustained), while TCP traffic is shaped on a different policy.
- **Practical takeaway for LLM inference (KV cache transfer)**: chunk transfers to ≤64KB, use RDMA write — avoids both the cloud QoS shaping and the RNR jitter on send/recv. Phase 4's `KVPool` design is built on this principle.

### Phase 3 — Ring All-Reduce (Azure VM, TCP backend, loopback)

| Benchmark | Min | Median | p99 |
|---|---|---|---|
| `ring_allreduce` TCP, N=2, 1024 floats (4KB) | 45 μs | 95 μs | 121 μs |

> TCP loopback baseline measured on Azure VM (historical, before committing to Alibaba Cloud eRDMA). RDMA backend benchmark pending eRDMA two-machine run.

### Phase 4 — Remote KV Cache (slot_size=4096B)

| Benchmark | Environment | Min | Median | p99 | Max |
|---|---|---|---|---|---|
| `kv_bench` prefill (RDMA write) | Azure MANA RoCE, loopback | 8.41 μs | 8.50 μs | 10.28 μs | 59.89 μs |
| `kv_bench` decode (RDMA read) | SoftRoCE, loopback | 8.80 μs | 8.90 μs | 9.20 μs | 25.40 μs |

Throughput (1000 iters × 4096B): prefill **0.44 GB/s / 3.79 Gbps** · decode **0.42 GB/s / 3.63 Gbps**

> Historical: prefill measured on Azure MANA RoCE hardware before committing to Alibaba Cloud eRDMA; decode on SoftRoCE loopback. Will be re-measured on eRDMA two-machine setup as part of Phase 4 cleanup.

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
│   │   └── rdma_common.h            # rai_qp_t, rai_mr_t, rai_conn_info_t
│   ├── src/
│   │   ├── rdma_qp.c                # rai_qp_destroy (idempotent teardown)
│   │   ├── rdma_mr.c                # MR register / deregister
│   │   ├── rdma_ops.c               # post_send / post_recv / post_write / poll_cq
│   │   ├── rdma_cm_connect.c        # rdma_cm-based connect (server/client/listen/accept)
│   │   └── rdma_connect.c           # rai_oob_listen / accept / connect (TCP for MR exchange)
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
│       └── backend_compare.cpp      # send/recv on RDMA + TCP, write on RDMA only
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
├── phase4_kv_cache/                 # C++17
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
sudo apt install build-essential cmake libibverbs-dev librdmacm-dev ibverbs-utils rdma-core

# Optional: SoftRoCE for development (production target is Alibaba eRDMA)
sudo apt install linux-modules-extra-$(uname -r)
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0

# Build
mkdir build && cd build
cmake .. && make
```

## Running Benchmarks

```bash
# Phase 4 — Remote KV Cache
cd build/phase4_kv_cache

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
