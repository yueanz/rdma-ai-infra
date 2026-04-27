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

/* Build pd + cq + qp on cm_id (which provides the verbs context).
 * On failure, qp may be partially populated; caller must rai_qp_destroy(qp)
 * for cleanup (the destroy is idempotent and handles any partial state). */
static int build_qp_from_cm(rai_qp_t *qp, struct rdma_cm_id *id)
{
    qp->pd = ibv_alloc_pd(id->verbs);
    if (!qp->pd) { LOG_ERR("ibv_alloc_pd failed"); return -1; }

    qp->cq = ibv_create_cq(id->verbs, CQ_DEPTH, NULL, NULL, 0);
    if (!qp->cq) { LOG_ERR("ibv_create_cq failed"); return -1; }

    struct ibv_qp_init_attr attr = {
        .send_cq = qp->cq, .recv_cq = qp->cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 128, .max_recv_wr = 128,
                 .max_send_sge = 1, .max_recv_sge = 1 },
    };
    if (rdma_create_qp(id, qp->pd, &attr) != 0) {
        LOG_ERR("rdma_create_qp failed"); return -1;
    }
    qp->qp = id->qp;
    return 0;
}

static int cm_wait_event(struct rdma_event_channel *ec, enum rdma_cm_event_type expected, struct rdma_cm_event **out_event) {
    if (rdma_get_cm_event(ec, out_event) != 0) {
        LOG_ERR("cm_wait_event failed: rdma_get_cm_event failed");
        return -1;
    }
    if ((*out_event)->event != expected) {
        LOG_ERR("cm_wait_event failed: expected %s, got %s", rdma_event_str(expected), rdma_event_str((*out_event)->event));
        rdma_ack_cm_event(*out_event);   /* must ack even on error or events leak */
        *out_event = NULL;
        return -1;
    }
    return 0;
}

int rai_cm_server(rai_qp_t *qp, rai_mr_t *mr, size_t mr_size, int port)
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
    if (!ec) {
        LOG_ERR("rdma_create_event_channel failed"); 
        goto out; 
    }
    qp->ec = ec;

    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP)) {
        LOG_ERR("rdma_create_id failed");
        goto out;
    }

    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr)) {
        LOG_ERR("rdma_bind_addr failed");
        goto out;
    }
    if (rdma_listen(listen_id, 1)) {
        LOG_ERR("rdma_listen failed");
        goto out;
    }
    LOG_INFO("waiting for rdma_cm connection on port %d", port);

    if (cm_wait_event(ec, RDMA_CM_EVENT_CONNECT_REQUEST, &event)) goto out;
    conn_id = event->id;
    qp->cm_id = conn_id;  /* transfer ownership early so cleanup catches it */
    if (event->param.conn.private_data_len >= sizeof(mr_info_t))
        memcpy(&peer_mr, event->param.conn.private_data, sizeof(mr_info_t));
    rdma_ack_cm_event(event);
    event = NULL;

    if (build_qp_from_cm(qp, conn_id)) goto out;
    if (rai_mr_reg(qp, mr, mr_size)) { LOG_ERR("rai_mr_reg failed"); goto out; }

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

    /* qp->qp / cm_id / ec already set above; just record the MR exchange info */
    qp->local.addr  = local_mr.addr;
    qp->local.rkey  = local_mr.rkey;
    qp->remote.addr = peer_mr.addr;
    qp->remote.rkey = peer_mr.rkey;

    ret     = 0;

out:
    if (listen_id)
        rdma_destroy_id(listen_id);
    if (ret != 0) {
        rai_mr_dereg(mr);   /* MR depends on PD — must dereg before qp_destroy */
        rai_qp_destroy(qp);
    }
    return ret;
}

int rai_cm_client(rai_qp_t *qp, rai_mr_t *mr, size_t mr_size,
                  const char *server_ip, int port)
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
    qp->ec = ec;

    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) {
        LOG_ERR("rdma_create_id failed"); goto out;
    }
    qp->cm_id = id;

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

    if (build_qp_from_cm(qp, id)) goto out;
    if (rai_mr_reg(qp, mr, mr_size)) { LOG_ERR("rai_mr_reg failed"); goto out; }

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

    /* qp->qp / cm_id / ec already set above; just record the MR exchange info */
    qp->local.addr  = local_mr.addr;
    qp->local.rkey  = local_mr.rkey;
    qp->remote.addr = peer_mr.addr;
    qp->remote.rkey = peer_mr.rkey;

    ret = 0;

out:
    if (ret != 0) {
        rai_mr_dereg(mr);   /* MR depends on PD — must dereg before qp_destroy */
        rai_qp_destroy(qp);
    }
    return ret;
}

int rai_cm_listen_qp(rai_qp_t *qp, int port, int *mr_listen_fd) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listen_id = NULL;
    struct rdma_cm_id *conn_id = NULL;
    struct rdma_cm_event *event = NULL;
    struct sockaddr_in addr  = {0};
    int ret = -1;

    ec = rdma_create_event_channel();
    if (ec == NULL) {
        LOG_ERR("rai_cm_listen_qp failed: rdma_create_event_channel failed");
        goto out;
    }
    qp->ec = ec;

    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP) != 0) {
        LOG_ERR("rai_cm_listen_qp failed: rdma_create_id failed");
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
    qp->cm_id = conn_id;  /* transfer ownership early so cleanup catches it */
    rdma_ack_cm_event(event);
    event = NULL;

    if (build_qp_from_cm(qp, conn_id) != 0) {
        LOG_ERR("rai_cm_listen_qp failed: build_qp_from_cm failed");
        goto out;
    }

    if (rai_oob_listen(port + 1, mr_listen_fd) != 0) {
        LOG_ERR("rai_cm_listen_qp failed: rai_oob_listen failed");
        goto out;
    }

    /* qp->qp / cm_id / ec already set above */
    ret = 0;
out:
    if (listen_id)
        rdma_destroy_id(listen_id);
    if (ret != 0)
        rai_qp_destroy(qp);
        
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

int rai_cm_connect_qp(rai_qp_t *qp, const char *server_ip, int port) {
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
    qp->ec = ec;

    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: rdma_create_id failed");
        goto out;
    }
    qp->cm_id = id;

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

    if (build_qp_from_cm(qp, id) != 0) {
        LOG_ERR("rai_cm_connect_qp failed: build_qp_from_cm failed");
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

    /* qp->qp / cm_id / ec already set above */
    ret = 0;
out:
    if (ret != 0)
        rai_qp_destroy(qp);
    return ret;
}
