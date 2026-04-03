#pragma once
#include <infiniband/verbs.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * rdma_conn_info_t
 *
 * The information we exchange with the remote peer out-of-band (via TCP)
 * before RDMA communication can begin. Both sides need each other's info
 * to bring their Queue Pairs to a connected state.
 */
typedef struct rdma_conn_info
{
    uint32_t      qpn;   /* Queue Pair Number — uniquely identifies a QP on a host */
    uint32_t      psn;   /* Packet Sequence Number — starting sequence number,
                            used to detect out-of-order or lost packets */
    union ibv_gid gid;   /* Global Identifier — a 128-bit address (like an IPv6 addr)
                            used to route packets in RoCE networks */
    uint64_t      addr;  /* Virtual address of the remote memory buffer,
                            needed for one-sided RDMA write/read */
    uint32_t      rkey;  /* Remote Key — authorizes the remote side to access
                            our memory region; obtained from ibv_reg_mr */
} rdma_conn_info_t;

/*
 * rdma_ctx_t
 *
 * The top-level RDMA context. Owns the device handle, protection domain,
 * and completion queue. One instance per process.
 */
typedef struct rdma_ctx
{
    struct ibv_context *ctx;       /* Handle to the opened RDMA device (HCA) */
    struct ibv_pd      *pd;        /* Protection Domain — a security container;
                                      QPs and MRs must belong to the same PD
                                      to communicate with each other */
    struct ibv_cq      *cq;        /* Completion Queue — the NIC posts a completion
                                      entry here when a send/recv/write finishes;
                                      we poll this to know when operations are done */
    int                 port;      /* Physical port on the HCA, usually 1 */
    int                 gid_index; /* Index into the GID table for this port;
                                      for RoCE (SoftRoCE), typically index 1 */
} rdma_ctx_t;


/*
 * rdma_mr_t
 *
 * A registered memory region. Before the NIC can DMA into/out of a buffer,
 * the buffer must be pinned in physical memory and registered with the HCA.
 * Registration gives us lkey (for local access) and rkey (for remote access).
 */
typedef struct rdma_mr
{
    struct ibv_mr *mr;   /* The registered MR handle; mr->lkey and mr->rkey
                            are the access keys the NIC uses */
    void          *buf;  /* Pointer to the backing memory buffer */
    size_t         size; /* Size of the buffer in bytes */
    int            owns_buf;
} rdma_mr_t;


/*
 * rdma_qp_t
 *
 * Represents one reliable connected (RC) Queue Pair — i.e., one connection
 * to a remote peer. Contains the QP handle and both sides' connection info.
 */
typedef struct rdma_qp
{
    struct ibv_qp    *qp;     /* The Queue Pair handle; has a Send Queue (SQ)
                                 and Receive Queue (RQ) inside */
    rdma_conn_info_t  local;  /* Our own connection info (filled at QP creation) */
    rdma_conn_info_t  remote; /* Peer's connection info (filled after OOB exchange) */
} rdma_qp_t;

int rdma_ctx_init(rdma_ctx_t *ctx, int port, int gid_index);
void rdma_ctx_destroy(rdma_ctx_t *ctx);
int rdma_mr_reg(rdma_ctx_t *ctx, rdma_mr_t *mr, size_t size);
int rdma_mr_reg_external(rdma_ctx_t *ctx, rdma_mr_t *mr, void *buf, size_t size);
void rdma_mr_dereg(rdma_mr_t *mr);
int rdma_qp_create(rdma_ctx_t *ctx, rdma_qp_t *qp);
int rdma_qp_init(rdma_ctx_t *ctx, rdma_qp_t *qp);
void rdma_qp_destroy(rdma_qp_t *qp);
int rdma_exchange_info_server(rdma_qp_t *qp, int port);
int rdma_exchange_info_client(rdma_qp_t *qp, const char *server_ip, int port);
int rdma_qp_connect(rdma_ctx_t *ctx, rdma_qp_t *qp);
int rdma_post_send(rdma_qp_t *qp, rdma_mr_t *mr, uint32_t size, uint64_t id);
int rdma_post_recv(rdma_qp_t *qp, rdma_mr_t *mr, uint32_t size, uint64_t id);
int rdma_post_write(rdma_qp_t *qp, rdma_mr_t *mr, uint32_t size,
                    uint32_t send_flags, uint64_t remote_addr, uint32_t rkey);
int rdma_poll_cq(rdma_ctx_t *ctx, uint64_t *wr_id);
#ifdef __cplusplus
}
#endif
