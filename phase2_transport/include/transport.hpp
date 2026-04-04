#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {
#include "logging.h"
}

/*
 * BufferHandle — opaque handle to a transport-registered memory buffer.
 *
 * The caller owns the underlying memory (addr). reg_buf() fills this struct
 * and records any backend-specific state in priv.
 *
 *   addr  — pointer to the start of the buffer (caller-allocated)
 *   size  — size of the buffer in bytes
 *   priv  — backend-specific metadata:
 *             RDMA: pointer to rdma_mr_t (ibv_mr* + lkey/rkey)
 *             TCP:  nullptr (no registration needed)
 */
struct BufferHandle
{
    void   *addr;
    size_t  size;
    void   *priv;
};

class Transport {
public:
    Transport() = default;
    virtual ~Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    virtual int reg_buf(void *buf, size_t size, BufferHandle *out) = 0;
    virtual void dereg_buf(BufferHandle *h) = 0;

    virtual int send_async(const BufferHandle *h, size_t len, uint64_t id) = 0;
    virtual int recv_async(BufferHandle *h, size_t len, uint64_t id) = 0;

    // One-sided write: local buffer → remote addr (RDMA only; TCP falls back to send)
    // remote_addr and rkey obtained from peer during handshake
    virtual int write_async(const BufferHandle *local,
                            uint64_t remote_addr, uint32_t rkey,
                            size_t len, uint64_t id) = 0;

    // Exchange buffer info with the peer out-of-band (over the existing connection).
    // Must be called after reg_buf() and before write_async().
    // RDMA: exchanges addr + rkey so either side can RDMA write into the other's buffer.
    // TCP:  exchanges addr only; rkey is set to 0 (unused).
    virtual int exchange_buf(const BufferHandle *local,
                             uint64_t *remote_addr, uint32_t *rkey) = 0;

    virtual int poll(uint64_t *completed_id) = 0;

    virtual int connect(const char *host, int port) = 0;
    virtual int listen(int port) = 0;
    virtual int accept() = 0;
    virtual void close() = 0;
};

Transport *create_rdma_transport();
Transport *create_tcp_transport();