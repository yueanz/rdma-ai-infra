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
 * Owns the QP, the PD/CQ created from the rdma_cm device, and the cm_id/ec
 * for connection lifetime. Single-connection-per-instance design.
 */
typedef struct rai_qp
{
    struct ibv_qp    *qp;     /* The Queue Pair handle (SQ + RQ) */
    struct ibv_pd    *pd;     /* Protection Domain — required for ibv_reg_mr */
    struct ibv_cq    *cq;     /* Completion Queue — polled to detect WR completion */
    void             *cm_id;  /* struct rdma_cm_id* — rdma_cm connection handle */
    void             *ec;     /* struct rdma_event_channel* */
    rai_conn_info_t  local;   /* Our own connection info */
    rai_conn_info_t  remote;  /* Peer's connection info */
} rai_qp_t;

int  rai_mr_reg(rai_qp_t *qp, rai_mr_t *mr, size_t size);
int  rai_mr_reg_external(rai_qp_t *qp, rai_mr_t *mr, void *buf, size_t size);
void rai_mr_dereg(rai_mr_t *mr);
void rai_qp_destroy(rai_qp_t *qp);
/* OOB TCP helpers — used by exchange_buf in Phase 2+ for MR addr/rkey exchange */
int rai_oob_listen(int port, int *listen_fd);
int rai_oob_accept(int listen_fd, rai_qp_t *qp);
int rai_oob_connect(rai_qp_t *qp, const char *server_ip, int port);
/* rdma_cm all-in-one connect: establishes connection, allocates pd/cq into qp,
   registers MR, and exchanges addr/rkey via private_data. */
int rai_cm_server(rai_qp_t *qp, rai_mr_t *mr, size_t mr_size, int port);
int rai_cm_client(rai_qp_t *qp, rai_mr_t *mr, size_t mr_size,
                  const char *server_ip, int port);
/* Split rdma_cm flow: listen returns at CONNECT_REQUEST with PD/CQ/QP set up
   (QP in INIT). Caller registers MR + posts recv WRs, then calls accept_qp. */
int rai_cm_listen_qp(rai_qp_t *qp, int port, int *mr_listen_fd);
int rai_cm_accept_qp(rai_qp_t *qp);
int rai_cm_connect_qp(rai_qp_t *qp, const char *server_ip, int port);
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
