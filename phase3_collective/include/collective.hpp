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
 *
 * The caller registers the MRs once and reuses them. Registration is not free:
 * measured on this box it runs 11 us for 4 KB up to 345 us for 16 MB, and the
 * benchmark holds five registrations at a time. At 1 MB that is 32 us apiece
 * against a 168 us all-reduce, so doing it per call would dominate.
 *
 *   r_h         — buf registered on world.right
 *   l_h         — buf registered on world.left
 *   stage_h     — stage registered on world.left
 *   buf, count  — input/output array
 *   stage       — 2 * count/world.size floats. Two halves, not one: on a
 *                 backend whose receives only post a work request, the next
 *                 round's receive goes up before this round's sum is folded
 *                 in, so the two cannot share a buffer. A sender arriving
 *                 while the peer is still summing would otherwise find no
 *                 receive posted and stall on an RNR NAK: at 1 MB that was
 *                 the difference between a 2013 us all-reduce and a 187 us
 *                 one.
 */
int ring_allreduce(World *w, BufferHandle *r_h, BufferHandle *l_h, BufferHandle *stage_h,
                    float *buf, float *stage, size_t count);

/*
 * One exchange with each neighbour, to line the ranks up before a benchmark
 * starts its clock.
 *
 * Ranks drift apart doing their own between-iteration work, and on the
 * posted-receive path one that arrives early sends into a queue the late one
 * has not armed yet — an RNR NAK, and a stall the timer then charges to the
 * collective. Without this the p99 was ~2 ms against a 190 us median.
 *
 * Not a full barrier for a world larger than two: it only establishes that
 * the two neighbours have arrived, which is all the ring itself talks to.
 * With two ranks the neighbour is the whole rest of the world, so there it
 * happens to be one.
 *
 * Takes its own scratch buffer registered on both neighbours, not the
 * collective's data buffer: the byte it exchanges would land in the payload.
 */
int world_barrier(World *w, BufferHandle *scratch_r, BufferHandle *scratch_l);