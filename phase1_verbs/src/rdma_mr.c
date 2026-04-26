#include "rdma_common.h"
#include "logging.h"
#include <stdlib.h>

int rai_mr_reg(rai_ctx_t *ctx, rai_mr_t *mr, size_t size) {
    void *buf;

    if (ctx == NULL || ctx->pd == NULL) {
        LOG_ERR("rai_mr_reg failed: ctx or ctx->pd is null");
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
    mr->owns_buf = 1;
    return 0;
}

int rai_mr_reg_external(rai_ctx_t *ctx, rai_mr_t *mr, void *buf, size_t size) {
    if (size == 0) {
        LOG_ERR("rai_mr_reg_external failed: size is 0");
        return -1;
    }
    if (ctx == NULL || ctx->pd == NULL) {
        LOG_ERR("rai_mr_reg_external failed: ctx or ctx->pd is null");
        return -1;
    }
    if (mr == NULL || buf == NULL) {
        LOG_ERR("rai_mr_reg_external failed: mr or buf is null");
        return -1;
    }
    mr->mr = ibv_reg_mr(ctx->pd, buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (mr->mr == NULL) {
        LOG_ERR("rai_mr_reg_external failed: ibv_reg_mr failed");
        return -1;
    }
    mr->buf = buf;
    mr->size = size;
    mr->owns_buf = 0;
    return 0;
}

void rai_mr_dereg(rai_mr_t *mr) {
    if (mr == NULL) {
        LOG_ERR("rdma memory region is null");
        return;
    }
    if (mr->mr != NULL && ibv_dereg_mr(mr->mr) != 0) {
        LOG_ERR("failed to deregister memory region");
    }
    if (mr->owns_buf)
        free(mr->buf);
    mr->size = 0;
    memset(mr, 0, sizeof(*mr));
}