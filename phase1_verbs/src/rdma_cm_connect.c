#include <rdma/rdma_cma.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include "rdma_common.h"
#include "logging.h"

#define CM_TIMEOUT_MS 3000
#define CQ_DEPTH      128

typedef struct {
    uint64_t addr;
    uint32_t rkey;
} __attribute__((packed)) mr_info_t;

/* Allocate pd and cq from the device associated with a cm_id.
   ctx->ctx is left NULL because the verbs context is owned by rdma_cm. */
static int setup_ctx_from_cm(rai_ctx_t *ctx, struct rdma_cm_id *id)
{
    ctx->pd = ibv_alloc_pd(id->verbs);
    if (!ctx->pd) { LOG_ERR("ibv_alloc_pd failed"); return -1; }
    ctx->cq = ibv_create_cq(id->verbs, CQ_DEPTH, NULL, NULL, 0);
    if (!ctx->cq) {
        ibv_dealloc_pd(ctx->pd);
        ctx->pd = NULL;
        LOG_ERR("ibv_create_cq failed");
        return -1;
    }
    return 0;
}

static int create_qp_on_id(struct rdma_cm_id *id, rai_ctx_t *ctx)
{
    struct ibv_qp_init_attr attr = {0};
    attr.send_cq       = ctx->cq;
    attr.recv_cq       = ctx->cq;
    attr.qp_type       = IBV_QPT_RC;
    attr.cap.max_send_wr  = 128;
    attr.cap.max_recv_wr  = 128;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    return rdma_create_qp(id, ctx->pd, &attr);
}

static int cm_wait_event(struct rdma_event_channel *ec, enum rdma_cm_event_type expected, struct rdma_cm_event **out_event) {
    if (rdma_get_cm_event(ec, out_event) != 0) {
        LOG_ERR("cm_wait_event failed: rdma_get_cm_event failed");
        return -1;
    }
    if ((*out_event)->event != expected) {
        LOG_ERR("cm_wait_event failed: expected %s, got %s", rdma_event_str(expected), rdma_event_str((*out_event)->event));
        return -1;
    }
    return 0;
}

int rai_cm_server(rai_ctx_t *ctx, rai_qp_t *qp, rai_mr_t *mr,
                   size_t mr_size, int port)
{
    struct rdma_event_channel *ec       = NULL;
    struct rdma_cm_id         *listen_id = NULL;
    struct rdma_cm_id         *conn_id   = NULL;
    struct rdma_cm_event      *event     = NULL;
    struct sockaddr_in         addr      = {0};
    struct rdma_conn_param     cp        = {0};
    mr_info_t peer_mr = {0}, local_mr;
    int ret = -1;

    ec = rdma_create_event_channel();
    if (!ec) { LOG_ERR("rdma_create_event_channel failed"); goto out; }

    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP)) {
        LOG_ERR("rdma_create_id failed"); goto out;
    }

    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr)) {
        LOG_ERR("rdma_bind_addr failed"); goto out;
    }
    if (rdma_listen(listen_id, 1)) {
        LOG_ERR("rdma_listen failed"); goto out;
    }
    LOG_INFO("waiting for rdma_cm connection on port %d", port);

    if (cm_wait_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST, &event)) goto out;
    conn_id = event->id;
    if (event->param.conn.private_data_len >= sizeof(mr_info_t))
        memcpy(&peer_mr, event->param.conn.private_data, sizeof(mr_info_t));
    rdma_ack_cm_event(event);
    event = NULL;

    if (setup_ctx_from_cm(ctx, conn_id)) goto out;
    if (rai_mr_reg(ctx, mr, mr_size)) { LOG_ERR("rai_mr_reg failed"); goto out; }
    if (create_qp_on_id(conn_id, ctx)) { LOG_ERR("rdma_create_qp failed"); goto out; }

    /* Pre-post one recv WR before accepting so it is in the QP before the
     * client can possibly send.  Callers must not post an additional recv
     * for their first iteration — the completion from this WR is it. */
    {
        struct ibv_recv_wr wr = {0}, *bad;
        struct ibv_sge sge = {
            .addr   = (uint64_t)(uintptr_t)mr->buf,
            .length = (uint32_t)mr_size,
            .lkey   = mr->mr->lkey,
        };
        wr.sg_list = &sge;
        wr.num_sge = 1;
        if (ibv_post_recv(conn_id->qp, &wr, &bad) != 0) {
            LOG_ERR("ibv_post_recv initial recv failed");
            goto out;
        }
    }

    local_mr.addr          = (uint64_t)(uintptr_t)mr->mr->addr;
    local_mr.rkey          = mr->mr->rkey;
    cp.private_data        = &local_mr;
    cp.private_data_len    = sizeof(local_mr);
    cp.responder_resources = 1;
    cp.initiator_depth     = 1;
    if (rdma_accept(conn_id, &cp)) { LOG_ERR("rdma_accept failed"); goto out; }

    /* Set min_rnr_timer to max (491ms × 7 retries = ~3.4s) immediately after
     * rdma_accept while the QP is in RTS — before client can send any data.
     * Prevents RNR retry exhaustion on SoftRoCE where OS scheduling may delay
     * the server's first post_recv past the default retry window (~9ms). */
    {
        struct ibv_qp_attr rnr_attr = {0};
        rnr_attr.min_rnr_timer = 31;
        if (ibv_modify_qp(conn_id->qp, &rnr_attr, IBV_QP_MIN_RNR_TIMER) != 0)
            LOG_ERR("ibv_modify_qp min_rnr_timer failed (non-fatal)");
    }

    if (cm_wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, &event)) goto out;
    rdma_ack_cm_event(event);

    qp->qp          = conn_id->qp;
    qp->cm_id       = conn_id;
    qp->ec          = ec;
    qp->local.addr  = local_mr.addr;
    qp->local.rkey  = local_mr.rkey;
    qp->remote.addr = peer_mr.addr;
    qp->remote.rkey = peer_mr.rkey;
    conn_id = NULL;
    ec      = NULL;
    ret     = 0;

out:
    if (listen_id) rdma_destroy_id(listen_id);
    if (conn_id)   { if (conn_id->qp) rdma_destroy_qp(conn_id); rdma_destroy_id(conn_id); }
    if (ec)        rdma_destroy_event_channel(ec);
    return ret;
}

int rai_cm_client(rai_ctx_t *ctx, rai_qp_t *qp, rai_mr_t *mr,
                   size_t mr_size, const char *server_ip, int port)
{
    struct rdma_event_channel *ec    = NULL;
    struct rdma_cm_id         *id    = NULL;
    struct rdma_cm_event      *event = NULL;
    struct sockaddr_in         addr  = {0};
    struct rdma_conn_param     cp    = {0};
    mr_info_t local_mr, peer_mr = {0};
    int ret = -1;

    ec = rdma_create_event_channel();
    if (!ec) { LOG_ERR("rdma_create_event_channel failed"); goto out; }

    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) {
        LOG_ERR("rdma_create_id failed"); goto out;
    }

    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        LOG_ERR("invalid server_ip: %s", server_ip); goto out;
    }

    if (rdma_resolve_addr(id, NULL, (struct sockaddr *)&addr, CM_TIMEOUT_MS)) {
        LOG_ERR("rdma_resolve_addr failed"); goto out;
    }
    if (cm_wait_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, &event)) goto out;
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(id, CM_TIMEOUT_MS)) {
        LOG_ERR("rdma_resolve_route failed"); goto out;
    }
    if (cm_wait_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED, &event)) goto out;
    rdma_ack_cm_event(event);

    if (setup_ctx_from_cm(ctx, id)) goto out;
    if (rai_mr_reg(ctx, mr, mr_size)) { LOG_ERR("rai_mr_reg failed"); goto out; }
    if (create_qp_on_id(id, ctx)) { LOG_ERR("rdma_create_qp failed"); goto out; }

    local_mr.addr          = (uint64_t)(uintptr_t)mr->mr->addr;
    local_mr.rkey          = mr->mr->rkey;
    cp.private_data        = &local_mr;
    cp.private_data_len    = sizeof(local_mr);
    cp.responder_resources = 1;
    cp.initiator_depth     = 1;
    if (rdma_connect(id, &cp)) { LOG_ERR("rdma_connect failed"); goto out; }

    if (cm_wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, &event)) goto out;
    if (event->param.conn.private_data_len >= sizeof(mr_info_t))
        memcpy(&peer_mr, event->param.conn.private_data, sizeof(mr_info_t));
    rdma_ack_cm_event(event);

    qp->qp          = id->qp;
    qp->cm_id       = id;
    qp->ec          = ec;
    qp->local.addr  = local_mr.addr;
    qp->local.rkey  = local_mr.rkey;
    qp->remote.addr = peer_mr.addr;
    qp->remote.rkey = peer_mr.rkey;
    id = NULL;
    ec = NULL;
    ret = 0;

out:
    if (id) { if (id->qp) rdma_destroy_qp(id); rdma_destroy_id(id); }
    if (ec) rdma_destroy_event_channel(ec);
    return ret;
}

int rai_cm_listen_qp(rai_ctx_t *ctx, rai_qp_t *qp, int port, int *mr_listen_fd) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listen_id = NULL;
    struct rdma_cm_id *conn_id = NULL;
    struct rdma_cm_event *event = NULL;
    struct sockaddr_in addr  = {0};
    int ret = -1;

    ec = rdma_create_event_channel();
    if (ec == NULL) {
        LOG_ERR("rdma_cm_listen_qp failed: ec is null");
        goto out;
    }

    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP) != 0) {
        LOG_ERR("rdma_cm_listen_qp failled: rdma_creat_id failed");
        goto out;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr) != 0) {
        LOG_ERR("rdma_cm_listen_qp failed: rdma_bind_addr failed");
        goto out;
    }

    if (rdma_listen(listen_id, 1) != 0) {
        LOG_ERR("rdma_cm_listen_qp failed: rdma_listen failed");
        goto out;
    }
    LOG_INFO("waiting for rdma_cm connection on port %d", port);
    
    if (cm_wait_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST, &event)) goto out;
    conn_id = event->id;
    rdma_ack_cm_event(event);
    event = NULL;

    if (setup_ctx_from_cm(ctx, conn_id) != 0) {
        LOG_ERR("rdma_cm_listen_qp failed: setup_ctx_from_cm failed");
        goto out;
    }

    if (create_qp_on_id(conn_id, ctx) != 0) {
        LOG_ERR("rdma_cm_listen_qp failed: create_qp_on_id failed");
        goto out;
    }

    if (rai_oob_listen(port + 1, mr_listen_fd) != 0) {
        LOG_ERR("rdma_cm_listen_qp failed: rai_oob_listen failed");
        goto out;
    }

    qp->qp = conn_id->qp;
    qp->cm_id = conn_id;
    qp->ec = ec;
    conn_id = NULL;
    ec = NULL;

    ret = 0;
out:
    if (ec)
        rdma_destroy_event_channel(ec);
    if (listen_id)
        rdma_destroy_id(listen_id);
    if (conn_id) {
        if (conn_id->qp)
            rdma_destroy_qp(conn_id);
        rdma_destroy_id(conn_id);
    }
        
    return ret;
}

int rai_cm_accept_qp(rai_qp_t *qp) {
    struct rdma_cm_id *conn_id = (struct rdma_cm_id *)qp->cm_id;
    struct rdma_event_channel *ec = (struct rdma_event_channel *)qp->ec;
    struct rdma_cm_event *event = NULL;

    struct rdma_conn_param cp = {0};

    cp.responder_resources = 1;  /* max concurrent inbound RDMA reads we'll serve */
    cp.initiator_depth     = 1;  /* max concurrent outbound RDMA reads we'll issue */
    if (rdma_accept(conn_id, &cp) != 0) {
        LOG_ERR("rai_cm_accept_qp failed: rdma_accept failed");
        return -1;
    }

    struct ibv_qp_attr rnr_attr = {0};
    rnr_attr.min_rnr_timer = 31;
    if (ibv_modify_qp(qp->qp, &rnr_attr, IBV_QP_MIN_RNR_TIMER) != 0) {
        LOG_ERR("ibv_modify_qp min_rnr_timer failed(non-fatal)");
    }

    if (cm_wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, &event)) return -1;
    rdma_ack_cm_event(event);
    return 0;
}

int rai_cm_connect_qp(rai_ctx_t *ctx, rai_qp_t *qp, const char *server_ip, int port) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *id = NULL;
    struct sockaddr_in addr  = {0};
    struct rdma_cm_event *event = NULL;
    struct rdma_conn_param cp = {0};
    int ret = -1;

    ec = rdma_create_event_channel();
    if (ec == NULL) {
        LOG_ERR("rai_cm_connect_qp failed: rdma_create_event_channel failed");
        goto out;
    }

    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: rdma_create_id failed");
        goto out;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        LOG_ERR("rai_cm_connect_qp failed: invalid server_ip: %s", server_ip);
        goto out;
    }

    if (rdma_resolve_addr(id, NULL, (struct sockaddr *)&addr, CM_TIMEOUT_MS) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: rdma_resolve_addr failed");
        goto out;
    }
    if (cm_wait_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, &event)) goto out;
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(id, CM_TIMEOUT_MS) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: rdma_resolve_route failed");
        goto out;
    }
    if (cm_wait_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED, &event)) goto out;
    rdma_ack_cm_event(event);

    if (setup_ctx_from_cm(ctx, id) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: setup_ctx_from_cm failed");
        goto out;
    }

    if (create_qp_on_id(id, ctx) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: create_qp_on_id failed");
        goto out;
    }

    cp.responder_resources = 1;
    cp.initiator_depth = 1;
    if (rdma_connect(id, &cp) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: rdma_connect failed");
        goto out;
    }

    if (cm_wait_event(ec, RDMA_CM_EVENT_ESTABLISHED, &event)) goto out;
    rdma_ack_cm_event(event);

    qp->qp = id->qp;
    qp->cm_id = id;
    qp->ec = ec;
    id = NULL;
    ec = NULL;

    ret = 0;
out:
    if (id) {
        if (id->qp)
            rdma_destroy_qp(id);
        rdma_destroy_id(id);
    }
    if (ec)
        rdma_destroy_event_channel(ec);
    return ret;
}
