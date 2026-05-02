# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` and `rdma_cm` (no wrappers, no frameworks), implementing two end-to-end workloads from raw verbs upward: a mini NCCL-style ring all-reduce and a vLLM-style remote KV cache (prefill via RDMA write, decode via RDMA read).

## Phase Status

- [x] **Phase 1** — RDMA Verbs Foundation (RC QP, MR, CQ, send/recv, RDMA write, benchmarks). Uses `rdma_cm` for connection setup — Alibaba eRDMA (iWARP), the production target, rejects manual `ibv_modify_qp` state transitions. Migration retrospective: [`docs/raw-verbs-evolution.md`](docs/raw-verbs-evolution.md).
- [x] **Phase 2** — Transport Abstraction Layer (RDMA + TCP backends via rdma_cm, send/recv + write benchmarks; TCP write omitted — no one-sided primitive)
- [x] **Phase 3** — Ring All-Reduce (chunked pipeline, ring reduce-scatter + all-gather, RDMA + TCP backends benchmarked on eRDMA two-machine setup)
- [x] **Phase 4** — Remote KV Cache (slab allocator over single MR, ctrl/data plane separation, prefill via RDMA write, decode via RDMA read)
- [ ] **Phase 5** — vLLM KVTransferAgent Integration (planned). Scope: pybind11 binding for the Transport layer, an `RdmaKVConnector` implementing vLLM's `KVConnectorBase_V1` interface, CPU tensor path as the first integration target, GPUDirect on future hardware.

## Benchmark Results

> All numbers below built with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` (the project default; `-O2 -g -DNDEBUG`).
>
> Phase 1 was run twice on the same Alibaba Cloud ECS pair — once with eRDMA (hardware RDMA), once with SoftRoCE (software RDMA over UDP, `rdma_rxe`). Same machines, same code, different RDMA device.

### Phase 1 — eRDMA (Alibaba Cloud ECS, two machines)

| Benchmark | Min | Median | p99 | Max |
|---|---|---|---|---|
| `lat_send_recv` (RTT) | 38.30 μs | 39.93 μs | 44.96 μs | 52.59 μs |
| `lat_rdma_write` (one-sided) | 30.32 μs | 31.46 μs | 36.24 μs | 42.41 μs |

| Benchmark | Config | Throughput |
|---|---|---|
| `bw_rdma_write` | 1 MB × 100 iters, depth=1 (burst) | 9.06 GiB/s / 77.83 Gbps |
| `bw_rdma_write` | 1 MB × 1000 iters, depth=1 (sustained) | 3.30 GiB/s / 28.37 Gbps |

64 KB depth sweep (1000 iters):

| depth | eRDMA | SoftRoCE |
|---|---|---|
| 1 | 1.63 GiB/s / 13.98 Gbps | 0.90 GiB/s / 7.71 Gbps |
| 2 | 3.01 GiB/s / 25.82 Gbps | 1.27 GiB/s / 10.91 Gbps |
| 4 | 4.71 GiB/s / 40.44 Gbps | 1.65 GiB/s / 14.17 Gbps |
| 8 | 5.74 GiB/s / 49.33 Gbps | — (UDP buffer overflow) |
| 16 | 6.72 GiB/s / 57.76 Gbps | — |
| 32 | WR_FLUSH_ERR | — |

**Key insights:**
- RDMA write is ~21% lower latency than send/recv (median: 31 vs 40 μs) — one-sided ops bypass server-side CPU entirely
- Burst vs sustained throughput gap (78 → 28 Gbps) reflects Alibaba Cloud eRDMA fabric QoS: short transfers run at line rate, sustained transfers are throttled to a committed rate
- eRDMA throughput scales near-linearly to depth=8, then diminishing returns as depth=16 saturates the NIC (~58 Gbps); depth=32 errors out with `WR_FLUSH_ERR` — a fabric-level ceiling, not the QP's `max_send_wr` (set to 128 in `rdma_cm_connect.c`).
- SoftRoCE caps out at depth=4 (14 Gbps); depth≥8 triggers UDP buffer overflow — 65 KB × 8 in-flight = 128 concurrent UDP packets exceeds the kernel receive buffer; eRDMA at depth=4 (40 Gbps) already outperforms SoftRoCE's ceiling

### Phase 1 — SoftRoCE (Alibaba Cloud ECS, two machines)

Same physical machines as above; eRDMA unloaded (`modprobe -r erdma`), SoftRoCE loaded on the same NIC (`rdma link add rxe0 type rxe netdev eth0`). Differences reflect software vs hardware RDMA only.

| Benchmark | Min | Median | p99 | Max |
|---|---|---|---|---|
| `lat_send_recv` (RTT) | 41.21 μs | 46.72 μs | 53.49 μs | 61.97 μs |
| `lat_rdma_write` (one-sided) | 39.31 μs | 40.06 μs | 43.25 μs | 50.69 μs |

**Key insights:**
- eRDMA is ~22% lower latency than SoftRoCE (RDMA write median: 31 vs 40 μs) on the same hardware
- 1 MB writes fail on SoftRoCE (`transport retry counter exceeded`) due to UDP fragmentation; 64 KB is the practical ceiling for rdma_rxe
- See depth sweep table in the eRDMA section above for full throughput comparison

### Phase 2 — backend_compare (Alibaba Cloud ECS, eRDMA, two machines)

Compares RDMA backend (rdma_cm + libibverbs) against TCP backend across send/recv and write semantics, at multiple message sizes.

> **Note**: Phase 2's RDMA backend originally used raw verbs OOB handshake. eRDMA (iWARP-based) rejects manual `ibv_modify_qp` state transitions, so the backend was migrated to rdma_cm — see [`docs/raw-verbs-evolution.md`](docs/raw-verbs-evolution.md) for the migration retrospective. The migrated path is what's measured below.

**send/recv latency (RTT, two-sided echo)**

| Backend | Size | Min | Median | p99 | Max | Throughput |
|---|---|---|---|---|---|---|
| RDMA | 4 KB | 23.42 | 25.10 | **87,806** | 90,814 | 0.01 Gbps |
| RDMA | 64 KB | 31.91 | 34.17 | 87.56† | 45,655 | 2.02 Gbps |
| RDMA | 1 MB | 119.78 | 181.25 | 617.90 | 45,550 | 23.34 Gbps |
| TCP | 4 KB | 28.70 | 30.26 | 37.89 | 45.61 | 1.07 Gbps |
| TCP | 64 KB | 84.75 | 105.99 | 115.66 | 127.46 | 4.93 Gbps |
| TCP | 1 MB | 185.34 | 198.42 | 286.02 | 4,211 | **37.84 Gbps** |

† RDMA send/recv p99 is **bimodal**: clean (~87 µs) when polling stays scheduled, spikes to tens of ms (max column) when shared-VM CPU scheduling preempts the server's user-space CQ polling loop. Same root cause as the tail jitter analyzed in Phase 3's "On the jitter" section — *not* RNR retry (the observed outliers don't match the 491 ms / retry quantum of `min_rnr_timer = 31`). Run-to-run variance is huge — same code, same environment.

**RDMA write latency (one-sided, no server CPU involvement)**

| Size | Min | Median | p99 | Max | Throughput |
|---|---|---|---|---|---|
| 4 KB | 16.87 | 18.11 | 22.16 | 25.35 | 1.82 Gbps |
| 64 KB | 21.32 | 23.29 | 31.13 | 64.20 | **22.36 Gbps** |
| 1 MB | 119.96 | 321.37 | 426.59 | 1,827 | 25.85 Gbps |

> TCP write is omitted: TCP has no one-sided primitive — any emulation either measures syscall time (completion ≠ remote received) or degenerates into 2-sided send/recv with explicit ACK.

**Key insights:**

- **At 4 KB (small messages)**: RDMA write median 18 µs vs TCP send/recv 30 µs — **RDMA wins on latency** by 40%, and write is even faster than RDMA send/recv (one-sided skips server CPU).
- **At 64 KB (medium)**: RDMA write 22 Gbps vs RDMA send/recv 2 Gbps — **same payload size, 11× gap from operation choice alone**. The send/recv throughput collapses because the polling-preempt tail (see footnote on the latency table) drags the average into milliseconds. RDMA write is immune — server CPU is not involved, so no polling loop to preempt.
- **At 1 MB (large)**: TCP send/recv (37.84 Gbps) **outperforms** all RDMA modes (~25 Gbps). Reason: Alibaba eRDMA fabric applies QoS shaping to RDMA traffic at ~25–28 Gbps sustained (matches Phase 1 `bw_rdma_write` 28 Gbps sustained), while TCP traffic is shaped on a different policy.
- **Practical takeaway for LLM inference (KV cache transfer)**: chunk transfers to ≤64 KB, use RDMA write — avoids both the cloud QoS shaping and the polling-preempt tail on send/recv. Phase 4's `KVPool` design is built on this principle.

### Phase 3 — Ring All-Reduce (Alibaba Cloud ECS, eRDMA, two machines)

`ring_allreduce` measured at 5 sizes spanning ~3 orders of magnitude (4 KB to 10 MB; 2,560× ratio). RDMA wins on median at every size; the tail tells a more interesting story.

| count (floats) | bytes | RDMA median | RDMA p99 | TCP median | TCP p99 | Speedup (median) |
|---|---|---|---|---|---|---|
| 1,024 | 4 KB | 38 μs | 47 μs | 62 μs | 70 μs | 1.64× |
| 4,096 | 16 KB | 42 μs | 91 ms † | 77 μs | 83 μs | 1.83× |
| 65,536 | 256 KB | 102 μs | 235 μs | 241 μs | 264 μs | **2.37×** |
| 262,144 | 1 MB | 252 μs | 51 ms † | 318 μs | 332 μs | 1.26× |
| 2,621,440 | 10 MB | 2,009 μs ‡ | 3–48 ms § | 2,385 μs | 3,073 μs | 1.19× |

† RDMA p99 ≈ max → cluster of 2–3 consecutive bad iters in the tail (cloud-VM scheduling pause).
‡ RDMA 10 MB has high run-to-run variance (5 runs span 1,379–2,384 μs); reported value is cross-run median. TCP at 10 MB shows < 4% spread across runs.
§ Within a single run, p99 swings between ~3 ms (jitter-cluster-free) and ~48 ms (cluster hit).

**Three-regime pattern:**

- **Small messages (4–16 KB)**: RDMA's median advantage is ~1.7× — kernel-bypass saves the per-iter syscall cost. But the tail is fully dominated by jitter: per-iter work (~40 μs) is so small that any hypervisor preempt (tens of ms) ends up sitting on top of it.
- **Sweet spot (256 KB)**: both backends' tails are clean. Per-iter work (~100 μs) is large enough to amortize over the polling loop, but still under eRDMA's bandwidth cap. RDMA gets its biggest relative edge here (**2.37×**).
- **Bandwidth-bound (1–10 MB)**: RDMA's median edge holds at ~1.2× — eRDMA's QoS shapes sustained traffic to ~28 Gbps (matches Phase 1's `bw_rdma_write` ceiling), and TCP runs near line speed too. Jitter clusters reappear at 10 MB.

**On the jitter:**

At 4 KB / 16 KB / 10 MB, **p99 ≈ max**, meaning ≥2 consecutive iters land in the tail — a single hypervisor preempt event taking out a small cluster. At 256 KB / 1 MB the tail is mostly isolated single outliers. **Root cause is shared-VM CPU scheduling**: RDMA send/recv uses user-space CQ polling, so a vCPU preempt stalls the loop directly; TCP send/recv blocks in the kernel and gets re-scheduled. Not RNR retry — `min_rnr_timer = 31` would be 491 ms per retry, doesn't fit.

**On run-to-run variance**: at 10 MB, 5 RDMA runs landed at 1,379 / 1,664 / 2,009 / 2,271 / 2,384 μs (~70% spread). TCP at the same size: 2,350 / 2,385 / 2,445 μs (~4% spread). Single-run RDMA medians at large sizes aren't reliable as portfolio numbers — cross-run sampling matters. The same 70% spread is presumably present at smaller sizes but the absolute drift is masked by the lower per-iter cost.

**No TCP crossover** like Phase 2 backend_compare 1 MB showed. Ring all-reduce chunks the buffer into `count/N` pieces (5 MB at N=2 for 10 MB total) and the per-WR transfer bursts at NIC line rate; eRDMA's 28 Gbps QoS only shapes *sustained* streams (Phase 1's back-to-back 1,000-iter `bw_rdma_write`), not the gappy 1-iter-at-a-time pattern in ring all-reduce.

**The fix that made all this work**: `ring_allreduce` initially registered 3 MRs per call (`ScopedBuffer` inside the function). On eRDMA, `ibv_reg_mr` is ~10 ms, so each iteration spent ~30 ms in registration alone. Refactored to NCCL-style: caller pre-registers MRs once before the timed loop and passes `BufferHandle*` into `ring_allreduce`. Per-iter cost dropped from ~30 ms to ~50 μs (**600× faster**). Pre-fix, RDMA was ~540× *slower* than TCP at small sizes; post-fix, RDMA is ~2× *faster* than TCP — the bug effectively flipped the sign of the comparison.

> Production lesson: RDMA's user-space polling model is sensitive to OS scheduling jitter. NCCL/UCX deployments isolate CPU cores (`isolcpus`, cgroup pinning) or run on bare metal to avoid this. On shared cloud VMs the jitter is fundamental — visible only in the tail, but visible.

### Phase 4 — Remote KV Cache (Alibaba Cloud ECS, eRDMA, two machines)

`kv_bench` measured at 5 slot sizes spanning the typical vLLM KV-block range (4 KB → 1 MB).

**prefill (RDMA write to remote slot)**

| slot_size | Min | Median | p99 | Max | Throughput |
|---|---|---|---|---|---|
| 4 KB | 16.93 μs | **17.77 μs** | 22.58 μs | 33.19 μs | 1.81 Gbps |
| 16 KB | 18.14 μs | **19.16 μs** | 23.73 μs | 27.69 μs | 6.70 Gbps |
| 64 KB | 22.05 μs | **23.61 μs** | 28.56 μs | 34.66 μs | 22.12 Gbps |
| 256 KB | 29.43 μs | **31.53 μs** | 149.97 μs | 1185.74 μs | **41.20 Gbps** ⬆ peak |
| 1 MB | 64.15 μs | **314.02 μs** | 426.93 μs | 5503.09 μs | 28.23 Gbps |

**decode (RDMA read from remote slot)**

| slot_size | Min | Median | p99 | Max | Throughput |
|---|---|---|---|---|---|
| 4 KB | 18.28 μs | **20.21 μs** | 24.05 μs | 67.92 μs | 1.62 Gbps |
| 16 KB | 19.46 μs | **21.42 μs** | 24.50 μs | 29.02 μs | 6.10 Gbps |
| 64 KB | 22.88 μs | **24.67 μs** | 30.42 μs | 208.30 μs | 20.90 Gbps |
| 256 KB | 31.02 μs | **33.15 μs** | 142.76 μs | 1317.69 μs | 41.21 Gbps |
| 1 MB | 98.83 μs | **313.90 μs** | 435.00 μs | 4532.94 μs | 28.24 Gbps |

**Key insights:**

- **Three regimes in the scaling curve**:
  - **4–64 KB (latency-bound)**: median almost constant (18→23 μs), throughput scales near-linearly. Protocol overhead dominates, not bandwidth.
  - **64–256 KB (bandwidth-bound)**: throughput peaks at ~41 Gbps, hitting the eRDMA NIC's per-stream ceiling.
  - **1 MB (QoS-shaped)**: throughput drops back to **28.24 Gbps** — exactly matching Phase 1's `bw_rdma_write` 1 MB sustained (28.37 Gbps) and Phase 2's RDMA write (25.85 Gbps). This is Alibaba eRDMA's sustained-rate QoS shaping, **not** a Phase 4 limitation.

- **`KVPool` abstraction is zero-overhead**: at every size, `kv_bench` matches the raw RDMA numbers from Phase 1/2 within noise. The slab allocator + single pre-registered MR doesn't add a single μs to the data path.

- **RDMA read ≈ RDMA write**: decode is 0–14% slower than prefill, with the gap largest at small sizes (where one extra fetch round-trip is a meaningful fraction of total time) and shrinking to near-zero at 1 MB (bandwidth-bound, both sides hit the same fabric ceiling).

- **Clean tail at small sizes** (p99 ≤ 30 μs at ≤64 KB): unlike Phase 2/3 where `send/recv` showed 40–90 ms outliers from cloud-VM scheduling, Phase 4's one-sided write/read **doesn't involve the server's CPU** at all — server is just a passive doorbell target. Max occasionally spikes (e.g. 64 KB decode hit 208 μs from a single iter), but the spikes stay isolated and never cluster — p99 is the reliable description. **p99 jitter ramps up at ≥256 KB** (p99 ~150 μs at 256 KB, ~430 μs at 1 MB; max 1–5 ms across both) where transfers take long enough for cloud-fabric variance to show.

> **vLLM block size context**: a typical vLLM KV block (16 tokens × Llama-7B FP16) is ~256 KB; for larger models or block sizes it can hit 1 MB+. Phase 4's 256 KB result (31 μs median, 41 Gbps) is the sweet spot — small enough to dodge the QoS shaping, large enough to amortize per-op overhead. The decode path uses RDMA read so the consumer can pull blocks from the producer without the producer's CPU involvement, matching vLLM's disaggregated prefill/decode topology.

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
│   │   ├── rdma_ops.c               # post_send / post_recv / post_write / post_read / poll_cq
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

**Quick start** (auto-detects hardware RDMA / falls back to SoftRoCE, installs deps, builds):

```bash
bash scripts/setup.sh
```

**Manual** (Ubuntu 22.04):

```bash
sudo apt install build-essential cmake libibverbs-dev librdmacm-dev ibverbs-utils rdma-core

# Optional: SoftRoCE for development (production target is Alibaba eRDMA)
sudo apt install linux-modules-extra-$(uname -r)
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev eth0

# Build (defaults to RelWithDebInfo; pass -DCMAKE_BUILD_TYPE=Debug for gdb work)
cmake -B build && cmake --build build -j
```

## Running Benchmarks

All benchmarks are server/client pairs — start the server on one machine, run the client on another. For single-machine testing, use the RDMA-bound NIC's actual IP, **not `127.0.0.1`** (rxe/eRDMA is bound to a physical netdev like `eth0`; `127.0.0.1` routes through `lo` where no RDMA device is present, and the connection fails with `RDMA_CM_EVENT_CONNECT_ERROR`). Default port is 12345 unless noted.

### Phase 1 — raw RDMA primitives

```bash
cd build/phase1_verbs

# Send/recv RTT
./lat_send_recv                           # server
./lat_send_recv <server_ip>               # client

# RDMA write latency (one-sided)
./lat_rdma_write
./lat_rdma_write <server_ip>

# RDMA write throughput (server and client must use the same --size)
./bw_rdma_write --size 65536
./bw_rdma_write <server_ip> --size 65536 --iters 1000 --depth 16
```

### Phase 2 — Transport abstraction (RDMA vs TCP)

```bash
cd build/phase2_transport

./backend_compare rdma                    # server
./backend_compare rdma <server_ip>        # client (TCP: replace `rdma` with `tcp`)

# Use --port / --iters / --size for non-defaults; port is *not* positional.
./backend_compare rdma --port 23456
```

### Phase 3 — ring all-reduce (multi-process collective)

```bash
cd build/phase3_collective

# Both ranks need to start within ~60s of each other (connect retries every 100ms for 60s).
# Args: <rank> <world_size> <base_port> <host_0> <host_1> [...] [--rdma] [--count N] [--iters N]

# rank 0:
./allreduce_bench 0 2 12345 <host_0_ip> <host_1_ip> --rdma

# rank 1 (on the other machine):
./allreduce_bench 1 2 12345 <host_0_ip> <host_1_ip> --rdma
```

### Phase 4 — Remote KV cache

```bash
cd build/phase4_kv_cache

# Server: kv_server <port> <num_slots> <slot_size>
./kv_server 12345 16 4096

# Client (kv_bench wires up ctrl-on-port + data-on-port+2 internally):
./kv_bench <server_ip> 12345 [--iters <n>]
```

## Environment

- **OS**: Ubuntu 22.04 LTS
- **RDMA**: Alibaba Cloud ECS with eRDMA (production target); SoftRoCE (`rdma_rxe`) for local development
- **Network**: rdma_cm-based connection setup (works across iWARP / RoCE / IB)
- **Compiler**: GCC 11+, `-std=c11` (Phase 1), `-std=c++17` (Phase 2+)
