#pragma once
#include <infiniband/verbs.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * rai_conn_info_t
 *
 * The information we exchange with the remote peer out-of-band (via TCP)
 * before RDMA communication can begin. Both sides need each other's info
 * to bring their Queue Pairs to a connected state.
 */
typedef struct rai_conn_info
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
} rai_conn_info_t;

/*
 * rai_ctx_t
 *
 * The top-level RDMA context. Owns the device handle, protection domain,
 * and completion queue. One instance per process.
 */
typedef struct rai_ctx
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
} rai_ctx_t;


/*
 * rai_mr_t
 *
 * A registered memory region. Before the NIC can DMA into/out of a buffer,
 * the buffer must be pinned in physical memory and registered with the HCA.
 * Registration gives us lkey (for local access) and rkey (for remote access).
 */
typedef struct rai_mr
{
    struct ibv_mr *mr;   /* The registered MR handle; mr->lkey and mr->rkey
                            are the access keys the NIC uses */
    void          *buf;  /* Pointer to the backing memory buffer */
    size_t         size; /* Size of the buffer in bytes */
    int            owns_buf;
} rai_mr_t;


/*
 * rai_qp_t
 *
 * Represents one reliable connected (RC) Queue Pair — i.e., one connection
 * to a remote peer. Contains the QP handle and both sides' connection info.
 */
typedef struct rai_qp
{
    struct ibv_qp    *qp;     /* The Queue Pair handle; has a Send Queue (SQ)
                                 and Receive Queue (RQ) inside */
    void             *cm_id;  /* struct rdma_cm_id* — rdma_cm connection handle */
    void             *ec;     /* struct rdma_event_channel* */
    rai_conn_info_t  local;  /* Our own connection info */
    rai_conn_info_t  remote; /* Peer's connection info */
} rai_qp_t;

int rai_ctx_init(rai_ctx_t *ctx, int port, int gid_index);
void rai_ctx_destroy(rai_ctx_t *ctx);
int rai_mr_reg(rai_ctx_t *ctx, rai_mr_t *mr, size_t size);
int rai_mr_reg_external(rai_ctx_t *ctx, rai_mr_t *mr, void *buf, size_t size);
void rai_mr_dereg(rai_mr_t *mr);
int rai_qp_create(rai_ctx_t *ctx, rai_qp_t *qp);
int rai_qp_init(rai_ctx_t *ctx, rai_qp_t *qp);
void rai_qp_destroy(rai_qp_t *qp);
/* OOB TCP helpers — used by Phase 2 transport layer */
int rai_oob_listen(int port, int *listen_fd);
int rai_oob_accept(int listen_fd, rai_qp_t *qp);
int rai_oob_connect(rai_qp_t *qp, const char *server_ip, int port);
int rai_qp_connect(rai_ctx_t *ctx, rai_qp_t *qp);
/* rdma_cm all-in-one connect: establishes connection, allocates ctx pd/cq,
   registers MR, and exchanges addr/rkey via private_data */
int rai_cm_server(rai_ctx_t *ctx, rai_qp_t *qp, rai_mr_t *mr, size_t mr_size, int port);
int rai_cm_client(rai_ctx_t *ctx, rai_qp_t *qp, rai_mr_t *mr, size_t mr_size,
                   const char *server_ip, int port);
int rai_cm_listen_qp(rai_ctx_t *ctx, rai_qp_t *qp, int port, int *mr_listen_fd);
int rai_cm_accept_qp(rai_qp_t *qp);
int rai_cm_connect_qp(rai_ctx_t *ctx, rai_qp_t *qp, const char *server_ip, int port);
int rai_post_send(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset);
int rai_post_recv(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset);
int rai_post_write(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint32_t send_flags,
                    uint64_t remote_addr, uint32_t rkey, uint64_t id, size_t offset);
int rai_post_read(rai_qp_t *qp, rai_mr_t *mr, uint32_t size,
                    uint64_t remote_addr, uint32_t rkey, uint64_t id, size_t offset);
int rai_poll_cq(rai_ctx_t *ctx, uint64_t *wr_id);
#ifdef __cplusplus
}
#endif
