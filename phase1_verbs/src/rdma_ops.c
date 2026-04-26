#include "rdma_common.h"
#include "logging.h"
#include "timing.h"

int rai_post_send(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset) {
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;

    if (qp == NULL || qp->qp == NULL) {
        LOG_ERR("rai_post_send failed: qp or qp->qp is null");
        return -1;
    }
    if (mr == NULL || mr->mr == NULL) {
        LOG_ERR("rai_post_send failed: mr or mr->mr is null");
        return -1;
    }
    if (size == 0 || size > mr->size) {
        LOG_ERR("post size is 0 or post size exceeds the size of mr buf");
        return -1;
    }

    sge.addr = (uintptr_t)(mr->buf + offset);
    sge.length = size;
    sge.lkey = mr->mr->lkey;

    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    if (ibv_post_send(qp->qp, &wr, &bad_wr) != 0) {
        LOG_ERR("failed to post send");
        return -1;
    }
    return 0;
}

int rai_post_recv(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset) {
    struct ibv_sge sge = {0};
    struct ibv_recv_wr wr = {0};
    struct ibv_recv_wr *bad_wr = NULL;

    if (qp == NULL || qp->qp == NULL) {
        LOG_ERR("rai_post_recv failed: qp or qp->qp is null");
        return -1;
    }
    if (mr == NULL || mr->mr == NULL) {
        LOG_ERR("rai_post_recv failed: mr or mr->mr is null");
        return -1;
    }
    if (size == 0 || size > mr->size) {
        LOG_ERR("recv size is 0 or recv size exceeds the size of mr buf");
        return -1;
    }

    sge.addr = (uintptr_t)(mr->buf + offset);
    sge.length = size;
    sge.lkey = mr->mr->lkey;

    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    if (ibv_post_recv(qp->qp, &wr, &bad_wr) != 0) {
        LOG_ERR("ibv post recv failed");
        return -1;
    }

    return 0;
}

int rai_post_write(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint32_t send_flags,
                    uint64_t remote_addr, uint32_t rkey, uint64_t id, size_t offset) {
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;

    if (qp == NULL || qp->qp == NULL) {
        LOG_ERR("rai_post_write failed: qp or qp->qp is null");
        return -1;
    }
    if (mr == NULL || mr->mr == NULL) {
        LOG_ERR("rai_post_write failed: mr or mr->mr is null");
        return -1;
    }
    if (size == 0 || size > mr->size) {
        LOG_ERR("write size is 0 or write size exceeds the size of mr buf");
        return -1;
    }

    sge.addr = (uintptr_t)(mr->buf + offset);
    sge.length = size;
    sge.lkey = mr->mr->lkey;

    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = send_flags;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;

    if (ibv_post_send(qp->qp, &wr, &bad_wr) != 0) {
        LOG_ERR("rai_post_write failed: ibv_post_send failed");
        return -1;
    } 

    return 0;
}

int rai_post_read(rai_qp_t *qp, rai_mr_t *mr, uint32_t size,
                    uint64_t remote_addr, uint32_t rkey, uint64_t id, size_t offset) {
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;

    if (qp == NULL || qp->qp == NULL) {
        LOG_ERR("rai_post_read failed: qp or qp->qp is null");
        return -1;
    }
    if (mr == NULL || mr->mr == NULL) {
        LOG_ERR("rai_post_read failed: mr or mr->mr is null");
        return -1;
    }
    if (size == 0 || size > mr->size) {
        LOG_ERR("read size is 0 or read size exceeds the size of mr buf");
        return -1;
    }

    sge.addr = (uintptr_t)(mr->buf + offset);
    sge.length = size;
    sge.lkey = mr->mr->lkey;

    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;

    if (ibv_post_send(qp->qp, &wr, &bad_wr) != 0) {
        LOG_ERR("rai_post_read failed: ibv_post_send failed");
        return -1;
    } 

    return 0;
}

int rai_poll_cq(rai_ctx_t *ctx, uint64_t *wr_id) {
    int n;
    struct ibv_wc wc = {0};

    if (ctx == NULL || ctx->cq == NULL) {
        LOG_ERR("rai_poll_cq failed: ctx or ctx->cq is null");
        return -1;
    }
    while (1) {
        n = ibv_poll_cq(ctx->cq, 1, &wc);
        if (n < 0) {
            LOG_ERR("ibv poll completion queue failed");
            return -1;
        }
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                LOG_ERR("work completion error: %s", ibv_wc_status_str(wc.status));
                return -1;
            }
            if (wr_id != NULL) {
                *wr_id = wc.wr_id;
            }
            return 0;
        }
        CPU_RELAX();
    }
    return 0;
}