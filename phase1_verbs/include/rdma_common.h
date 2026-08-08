#pragma once
#include <infiniband/verbs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Send/recv queue depth every QP is created with. Caps how many WRs a caller
 * can keep in flight before it must reap completions. */
#define RAI_QP_MAX_WR 128

/*
 * How the connection is established. The data path (post_send/recv/write/read,
 * poll_cq) is identical in both modes — only setup differs. CM is 0 so a
 * zero-initialized rai_qp_t defaults to it.
 */
typedef enum rai_conn_mode
{
    RAI_CONN_CM = 0,    /* librdmacm drives the QP state machine */
    RAI_CONN_VERBS,     /* we walk INIT → RTR → RTS by hand */
} rai_conn_mode_t;

/*
 * Exchanged out-of-band. The OOB helpers send/recv this struct verbatim
 * (sizeof-driven), so a field added here automatically goes on the wire.
 * addr/rkey are used by both modes; qpn/psn/gid only by RAI_CONN_VERBS.
 * Field order avoids padding; no endianness conversion (same-arch peers).
 */
typedef struct rai_conn_info
{
    uint64_t      addr;  /* Virtual address of the remote memory buffer */
    union ibv_gid gid;   /* L3 address of the peer's port */
    uint32_t      rkey;  /* Remote Key — authorizes RDMA access to that buffer */
    uint32_t      qpn;   /* Queue Pair Number */
    uint32_t      psn;   /* Starting Packet Sequence Number */
} rai_conn_info_t;

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
 * One reliable connected (RC) Queue Pair = one connection to a remote peer.
 * Single-connection-per-instance design. `mode` exists to answer one
 * question: who owns qp/ctx, and therefore how to tear them down.
 */
typedef struct rai_qp
{
    struct ibv_context *ctx;  /* set only when we opened it ourselves (VERBS),
                                 so non-NULL means we must ibv_close_device it */
    struct ibv_pd      *pd;   /* Protection Domain — required for ibv_reg_mr */
    struct ibv_cq      *cq;   /* Completion Queue — polled to detect WR completion */
    struct ibv_qp      *qp;   /* The Queue Pair handle (SQ + RQ) */

    rai_conn_mode_t     mode;

    void *cm_id;              /* struct rdma_cm_id*         — CM only */
    void *ec;                 /* struct rdma_event_channel* — CM only */

    int   port_num;           /* HCA physical port          — VERBS only */
    int   gid_index;          /* index into the GID table   — VERBS only */

    rai_conn_info_t  local;   /* Our own connection info */
    rai_conn_info_t  remote;  /* Peer's connection info */
} rai_qp_t;

int  rai_mr_reg(rai_qp_t *qp, rai_mr_t *mr, size_t size);
int  rai_mr_reg_external(rai_qp_t *qp, rai_mr_t *mr, void *buf, size_t size);
void rai_mr_dereg(rai_mr_t *mr);
void rai_qp_destroy(rai_qp_t *qp);
/* OOB TCP side-channel: one rai_conn_info_t per call, sending qp->local
   and filling qp->remote. */
int rai_oob_listen(int port, int *listen_fd);
int rai_oob_accept(int listen_fd, rai_qp_t *qp);
int rai_oob_connect(rai_qp_t *qp, const char *server_ip, int port);
/* rdma_cm flow: listen returns at CONNECT_REQUEST with PD/CQ/QP set up
   (QP in INIT). Caller registers MR + posts recv WRs, then calls accept_qp.
   mr_listen_fd receives an OOB TCP listener on port+1 for the MR addr/rkey
   exchange; pass NULL to skip it (two-sided send/recv needs no exchange). */
int rai_cm_listen_qp(rai_qp_t *qp, int port, int *mr_listen_fd);
int rai_cm_accept_qp(rai_qp_t *qp);
int rai_cm_connect_qp(rai_qp_t *qp, const char *server_ip, int port);
/* raw verbs flow: same shape as the rdma_cm one above, but qpn/psn/gid are
   exchanged over OOB TCP on `port` before listen/connect returns, and the
   INIT → RTR → RTS transitions are done by hand. RoCE v2 only. */
int rai_verbs_listen_qp(rai_qp_t *qp, int port, int *mr_listen_fd);
int rai_verbs_accept_qp(rai_qp_t *qp);
int rai_verbs_connect_qp(rai_qp_t *qp, const char *server_ip, int port);
int rai_post_send(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset);
int rai_post_recv(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset);
int rai_post_write(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint32_t send_flags,
                    uint64_t remote_addr, uint32_t rkey, uint64_t id, size_t offset);
int rai_post_read(rai_qp_t *qp, rai_mr_t *mr, uint32_t size,
                    uint64_t remote_addr, uint32_t rkey, uint64_t id, size_t offset);
int rai_poll_cq(rai_qp_t *qp, uint64_t *wr_id);
#ifdef __cplusplus
}
#endif
