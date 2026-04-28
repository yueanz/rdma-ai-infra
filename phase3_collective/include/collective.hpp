#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include "transport.hpp"

/*
 * World — represents the process group.
 * Each process has a rank (0..size-1) and knows its neighbors.
 */
struct World {
    int rank;                           // this process's rank
    int size;                           // total number of processes
    std::unique_ptr<Transport> left;    // connection to left neighbor
    std::unique_ptr<Transport> right;   // connection to right neighbor
};

/*
 * Initialize the world: connect to left and right neighbors via Transport.
 * host_list[i] = IP of rank i, known upfront via command line.
 * Each rank listens on base_port + rank, connects to neighbor's port.
 *
 * left  neighbor: rank (rank-1+size)%size
 * right neighbor: rank (rank+1)%size
 */
int world_init(World *w, int rank, int size,
               const char **host_list, int base_port,
               bool use_rdma);

/*
 * Ring all-reduce: float32 sum, in-place.
 * Caller pre-registers MRs once and reuses them across calls
 * (ibv_reg_mr is ~10ms; doing it in the hot loop kills throughput).
 *
 *   r_h         — buf registered on world.right
 *   l_h         — buf registered on world.left
 *   stage_h     — stage registered on world.left
 *   buf, count  — input/output array
 *   stage       — staging buffer of count/world.size floats
 */
int ring_allreduce(World *w, BufferHandle *r_h, BufferHandle *l_h, BufferHandle *stage_h,
                    float *buf, float *stage, size_t count);