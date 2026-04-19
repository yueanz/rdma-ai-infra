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
 * addr/rkey are set after MR registration and sent to clients
 * so they can issue RDMA write (prefill) and RDMA read (decode)
 * directly into the slot at offset = slot_idx * slot_size.
 */
struct KVPool {
    std::vector<char>    mem;        // owns the backing storage
    size_t               slot_size; // bytes per slot
    int                  num_slots; // total number of slots
    std::vector<int>     free_list; // indices of available slots
    uint64_t             addr;      // MR base address (exposed to clients)
    uint32_t             rkey;      // MR rkey (exposed to clients)
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