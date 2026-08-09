#include "rdma_common.h"
#include "logging.h"
#include "timing.h"

/* Argument checks and the single-entry gather list every post shares.
 * `op` names the caller so the error still says which one failed. */
static int prepare_sge(const char *op, rai_qp_t *qp, rai_mr_t *mr,
                       uint32_t size, size_t offset, struct ibv_sge *sge) {
    if (qp == NULL || qp->qp == NULL) {
        LOG_ERR("%s failed: qp or qp->qp is null", op);
        return -1;
    }
    if (mr == NULL || mr->mr == NULL) {
        LOG_ERR("%s failed: mr or mr->mr is null", op);
        return -1;
    }
    if (size == 0 || (size_t)size + offset > mr->size) {
        LOG_ERR("%s failed: size=%u offset=%zu exceeds mr_size=%zu",
                op, size, offset, mr->size);
        return -1;
    }
    sge->addr   = (uintptr_t)((char *)mr->buf + offset);
    sge->length = size;
    sge->lkey   = mr->mr->lkey;
    return 0;
}

int rai_post_send(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset) {
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;

    if (prepare_sge("rai_post_send", qp, mr, size, offset, &sge) != 0)
        return -1;

    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    if (size <= qp->max_inline)
        wr.send_flags |= IBV_SEND_INLINE;

    if (ibv_post_send(qp->qp, &wr, &bad_wr) != 0) {
        LOG_ERR("rai_post_send failed: ibv_post_send failed");
        return -1;
    }
    return 0;
}

int rai_post_recv(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint64_t id, size_t offset) {
    struct ibv_sge sge = {0};
    struct ibv_recv_wr wr = {0};
    struct ibv_recv_wr *bad_wr = NULL;

    if (prepare_sge("rai_post_recv", qp, mr, size, offset, &sge) != 0)
        return -1;

    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    if (ibv_post_recv(qp->qp, &wr, &bad_wr) != 0) {
        LOG_ERR("rai_post_recv failed: ibv_post_recv failed");
        return -1;
    }
    return 0;
}

int rai_post_write(rai_qp_t *qp, rai_mr_t *mr, uint32_t size, uint32_t send_flags,
                    uint64_t remote_addr, uint32_t rkey, uint64_t id, size_t offset) {
    struct ibv_sge sge = {0};
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;

    if (prepare_sge("rai_post_write", qp, mr, size, offset, &sge) != 0)
        return -1;

    wr.wr_id = id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = send_flags;
    if (size <= qp->max_inline)
        wr.send_flags |= IBV_SEND_INLINE;
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

    if (prepare_sge("rai_post_read", qp, mr, size, offset, &sge) != 0)
        return -1;

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

int rai_poll_cq(rai_qp_t *qp, uint64_t *wr_id) {
    int n;
    struct ibv_wc wc = {0};

    if (qp == NULL || qp->cq == NULL) {
        LOG_ERR("rai_poll_cq failed: qp or qp->cq is null");
        return -1;
    }
    while (1) {
        n = ibv_poll_cq(qp->cq, 1, &wc);
        if (n < 0) {
            LOG_ERR("rai_poll_cq failed: ibv_poll_cq failed");
            return -1;
        }
        if (n > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                LOG_ERR("rai_poll_cq failed: %s", ibv_wc_status_str(wc.status));
                return -1;
            }
            if (wr_id != NULL) {
                *wr_id = wc.wr_id;
            }
            return 0;
        }
        CPU_RELAX();
    }
}