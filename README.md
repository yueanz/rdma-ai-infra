# RDMA-Based AI Communication Infrastructure

A from-scratch implementation of RDMA communication primitives, transport abstractions, and collective operations — targeting the infrastructure layer of distributed AI training and inference systems.

Built with `libibverbs` (no wrappers, no frameworks), progressing from raw verbs to a mini NCCL-style all-reduce.

## Phase Status

- [x] **Phase 1** — RDMA Verbs Foundation (RC QP, MR, CQ, send/recv, RDMA write, benchmarks)
- [ ] **Phase 2** — Transport Abstraction Layer (RDMA + TCP backends, async pipeline) — *in progress*
- [ ] **Phase 3** — Ring All-Reduce (chunked pipeline, RDMA write + doorbell sync, TCP vs RDMA)
- [ ] **Phase 4b** — Remote KV Cache (RDMA-based, prefill/decode access pattern)

## Benchmark Results

> Measured on SoftRoCE (rdma_rxe) over loopback, DigitalOcean 2GB VM, Ubuntu 22.04.
> Real InfiniBand hardware would show ~1000x lower latency.

| Benchmark | Min | Median | p99 |
|---|---|---|---|
| `lat_send_recv` (RTT) | 1265 μs | 2000 μs | 4952 μs |
| `lat_rdma_write` (one-sided) | 4 μs | 6 μs | 45 μs |
| `bw_rdma_write` (throughput) | — | 1.5 Gbps | — |

**Key insight:** RDMA write is 300x lower latency than send/recv because it is one-sided — the receiver's CPU is not involved. Reliable delivery is guaranteed by the RC transport layer, not the application.

## Architecture

```
rdma-ai-infra/
│
├── common/                          # Pure C — linked by all phases
│   ├── include/
│   │   ├── timing.h                 # CLOCK_MONOTONIC nanosecond timer
│   │   ├── logging.h                # LOG_INFO / LOG_ERR / LOG_DEBUG macros
│   │   └── cli.h
│   └── src/
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
│   │   ├── transport.hpp            # Transport pure virtual base class
│   │   ├── rdma_backend.hpp         # RDMA implementation (calls phase1 via extern "C")
│   │   └── tcp_backend.hpp          # TCP implementation
│   ├── src/
│   │   ├── rdma_backend.cpp
│   │   └── tcp_backend.cpp
│   └── bench/
│       └── backend_compare.cpp      # same workload over both backends
│
├── phase3_collective/               # C++
│   ├── include/
│   │   └── collective.hpp
│   ├── src/
│   │   ├── world.cpp                # process group init, rendezvous
│   │   ├── ring_topo.cpp            # ring neighbor computation
│   │   ├── ring_allreduce.cpp       # chunked ring all-reduce (float32 sum)
│   │   └── double_buffer.cpp        # compute / communicate overlap
│   └── bench/
│       ├── allreduce_bench.cpp      # latency + BW across world sizes
│       └── tcp_vs_rdma.cpp          # backend comparison
│
├── phase4b_kv_cache/                # C++
│   ├── include/
│   │   └── kv_cache.hpp
│   ├── src/
│   │   ├── remote_allocator.cpp     # slab allocator over registered MRs
│   │   ├── kv_server.cpp
│   │   └── kv_client.cpp
│   └── bench/
│       ├── kv_bench.cpp             # small vs large object latency
│       └── inference_pattern.cpp    # prefill write + decode read pattern
│
├── tests/                           # Correctness tests (separate from bench)
│   ├── test_mr.c                    # MR register/deregister + content verify
│   ├── test_qp_connect.c            # QP build + send/recv correctness
│   └── test_transport.cpp           # RDMA vs TCP backend same output
│
├── scripts/
│   ├── setup_softroce.sh            # modprobe rdma_rxe + bind to eth0
│   └── run_bench.sh
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
```

## Environment

- OS: Ubuntu 22.04 LTS
- RDMA: SoftRoCE (`rdma_rxe`) over loopback for development; designed for real InfiniBand / RoCE hardware
- Compiler: GCC 11+, `-std=c11` (Phase 1), `-std=c++17` (Phase 2+)
