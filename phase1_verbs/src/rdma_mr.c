#include "rdma_common.h"
#include "logging.h"
#include <stdlib.h>

int rdma_mr_reg(rdma_ctx_t *ctx, rdma_mr_t *mr, size_t size) {
    void *buf;

    if (ctx == NULL) {
        LOG_ERR("rdma context is null");
        return -1;
    }
    if (mr == NULL) {
        LOG_ERR("rdma memory region is null");
        return -1;
    }
    buf = malloc(size);
    if (buf == NULL) {
        LOG_ERR("failed to allocate memory");
        return -1;
    }
    mr->mr = ibv_reg_mr(ctx->pd, buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (mr->mr == NULL) {
        LOG_ERR("failed to register memory region");
        free(buf);
        return -1;
    }
    mr->buf = buf;
    mr->size = size;
    return 0;
}

void rdma_mr_dereg(rdma_mr_t *mr) {
    if (mr == NULL) {
        LOG_ERR("rdma memory region is null");
        return;
    }
    if (mr->mr != NULL && ibv_dereg_mr(mr->mr) != 0) {
        LOG_ERR("failed to deregister memory region");
    }
    free(mr->buf);
    mr->size = 0;
}