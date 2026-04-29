#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>

enum { KV_MSG_ALLOC = 0, KV_MSG_FREE = 1, KV_MSG_META = 2 };

/*
 * KVPool — server-side slab allocator over a single registered MR.
 * Memory is divided into fixed-size slots; each slot holds one KV block
 * (e.g. one transformer layer's K and V tensors for one request).
 *
 * alloc: pop from free_list, return slot index (-1 if full)
 * free:  push slot index back onto free_list
 *
 * The MR's addr+rkey are exchanged with the client via exchange_buf at
 * connect time — the client computes per-slot offsets from base_addr.
 * Server itself never reads its own MR addr/rkey: the slab is the *target*
 * of client RDMA write/read; server's role is just to ALLOC/FREE slots.
 */
struct KVPool {
    std::vector<char>    mem;        // owns the backing storage
    size_t               slot_size; // bytes per slot
    int                  num_slots; // total number of slots
    std::vector<int>     free_list; // indices of available slots
};

/*
 * KVRemote — client-side view of the server's memory region.
 * Received from the server at connect time via exchange_buf.
 * Client computes slot offset as: base_addr + slot_idx * slot_size.
 */
struct KVRemote {
    uint64_t base_addr;  // server MR base address
    uint32_t rkey;       // server MR rkey for RDMA access
    size_t   slot_size;  // bytes per slot
    int      num_slots;  // total slots available
};

struct KVMeta {
    int    num_slots;
    size_t slot_size;
};

union CtrlBuf {
    int    msg[2];   // ALLOC/FREE msg
    KVMeta meta;      // META reply
};