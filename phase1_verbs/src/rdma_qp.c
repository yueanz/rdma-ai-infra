#include "rdma_common.h"
#include "logging.h"
#include <stdlib.h>
#include <string.h>

int rdma_qp_create(rdma_ctx_t *ctx, rdma_qp_t *qp) {
    struct ibv_qp_init_attr qp_init_attr  = {0};

    if (ctx == NULL) {
        LOG_ERR("rdma context is null");
        return -1;
    }
    if (qp == NULL) {
        LOG_ERR("rdma queue pair is null");
        return -1;
    }

    qp_init_attr.send_cq = ctx->cq;
    qp_init_attr.recv_cq = ctx->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;  // reliable connection
    qp_init_attr.cap.max_send_wr  = 128;
    qp_init_attr.cap.max_recv_wr  = 128;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    qp->qp = ibv_create_qp(ctx->pd, &qp_init_attr);
    if (qp->qp == NULL) {
        LOG_ERR("failed to create queue pair");
        return -1;
    }

    qp->local.qpn = qp->qp->qp_num;
    qp->local.psn = rand() & 0xffffff;
    if (ibv_query_gid(ctx->ctx, ctx->port, ctx->gid_index, &qp->local.gid) != 0) {
        LOG_ERR("failed to query gid");
        ibv_destroy_qp(qp->qp); 
        qp->qp = NULL;
        return -1;
    }

    return 0;
}

int rdma_qp_init(rdma_ctx_t *ctx, rdma_qp_t *qp) {
    int attr_mask = 0;
    struct ibv_qp_attr attr = {0};

    if (ctx == NULL) {
        LOG_ERR("rdma context is null");
        return -1;
    }
    if (qp == NULL) {
        LOG_ERR("rdma queue pair is null");
        return -1;
    }

    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = ctx->port;
    attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_READ |
                        IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(qp->qp, &attr, attr_mask) != 0) {
        LOG_ERR("failed to modify queue pair");
        return -1;
    }

    return 0;
}

int rdma_qp_connect(rdma_ctx_t *ctx, rdma_qp_t *qp) {
    int attr_mask = 0;
    struct ibv_qp_attr attr = {0};

    if (ctx == NULL) {
        LOG_ERR("rdma context is null");
        return -1;
    }
    if (qp == NULL) {
        LOG_ERR("rdma queue pair is null");
        return -1;
    }

    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = qp->remote.qpn;
    attr.rq_psn = qp->remote.psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = qp->remote.gid;
    attr.ah_attr.grh.sgid_index = ctx->gid_index;
    attr.ah_attr.grh.hop_limit = 0xff;
    attr.ah_attr.port_num = ctx->port;
    attr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    // init -> rtr
    if (ibv_modify_qp(qp->qp, &attr, attr_mask) != 0) {
        LOG_ERR("failed to modify queue pair");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = qp->local.psn;
    attr.max_rd_atomic = 1;
    attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;    

    // rtr -> rts
    if (ibv_modify_qp(qp->qp, &attr, attr_mask) != 0) {
        LOG_ERR("modify queue pair failed");
        return -1;
    }

    return 0;
}

void rdma_qp_destroy(rdma_qp_t *qp) {
    if (qp == NULL) {
        LOG_ERR(" rdma queue pair is null");
        return;
    }
    if (qp->qp && ibv_destroy_qp(qp->qp) != 0) {
        LOG_ERR("destroy queue pair failed");
    }
}