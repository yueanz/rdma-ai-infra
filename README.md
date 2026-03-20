rdma-ai-infra/
│
├── common/                          # 纯 C — 两边都能链
│   ├── include/
│   │   ├── timing.h
│   │   ├── logging.h
│   │   └── cli.h
│   └── src/
│       ├── timing.c
│       ├── logging.c
│       └── cli.c
│
├── phase1_verbs/                    # 纯 C ──────────────────────────
│   ├── include/
│   │   └── rdma_common.h
│   ├── src/
│   │   ├── rdma_context.c
│   │   ├── rdma_qp.c
│   │   ├── rdma_mr.c
│   │   ├── rdma_ops.c
│   │   └── rdma_connect.c
│   ├── bench/
│   │   ├── lat_send_recv.c
│   │   ├── lat_rdma_write.c
│   │   └── bw_rdma_write.c
│   └── CMakeLists.txt
│                                    # ── C / C++ 分界线 ──────────────
│
├── phase2_transport/                # C++ -std=c++17
│   ├── include/
│   │   ├── transport.hpp            # Transport 抽象基类
│   │   ├── rdma_backend.hpp
│   │   └── tcp_backend.hpp
│   ├── src/
│   │   ├── rdma_backend.cpp         # 内部调 phase1 的 C API（extern "C"）
│   │   └── tcp_backend.cpp
│   ├── bench/
│   │   └── backend_compare.cpp
│   └── CMakeLists.txt
│
├── phase3_collective/               # C++
│   ├── include/
│   │   └── collective.hpp
│   ├── src/
│   │   ├── world.cpp
│   │   ├── ring_topo.cpp
│   │   ├── ring_allreduce.cpp
│   │   ├── tree_allreduce.cpp
│   │   └── double_buffer.cpp
│   ├── bench/
│   │   ├── allreduce_bench.cpp
│   │   └── tcp_vs_rdma.cpp
│   └── CMakeLists.txt
│
├── phase4a_monitor/                 # C++
│   ├── include/
│   │   └── monitor.hpp
│   ├── src/
│   │   ├── sysfs_reader.cpp
│   │   ├── shm_ringbuf.cpp
│   │   └── daemon.cpp
│   ├── consumer/
│   │   ├── shm_reader.cpp
│   │   └── correlate.cpp
│   └── CMakeLists.txt
│
├── phase4b_kv_cache/                # C++
│   ├── include/
│   │   └── kv_cache.hpp
│   ├── src/
│   │   ├── remote_allocator.cpp
│   │   ├── kv_server.cpp
│   │   └── kv_client.cpp
│   ├── bench/
│   │   ├── kv_bench.cpp
│   │   └── inference_pattern.cpp
│   └── CMakeLists.txt
│
├── tests/
│   ├── test_mr.c                    # phase1 correctness — C
│   ├── test_qp_connect.c            # phase1 correctness — C
│   ├── test_ring_allreduce.cpp      # phase3+ — C++
│   └── test_kv_put_get.cpp
│
├── scripts/
│   ├── setup_softroce.sh
│   ├── run_bench.sh
│   └── plot_results.py
│
├── docs/
│   ├── phase1_notes.md
│   ├── phase2_design.md
│   ├── phase3_algorithm.md
│   └── phase4_arch.md
│
├── CMakeLists.txt
└── README.md
