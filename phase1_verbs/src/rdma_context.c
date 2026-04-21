#include "rdma_common.h"
#include "logging.h"
#include <stdlib.h>

#define CQ_DEPTH 128

int rdma_ctx_init(rdma_ctx_t *ctx, int port, int gid_index) {
    if (ctx == NULL) {
        LOG_ERR("rdma context is null");
        return -1;
    }
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (dev_list == NULL) {
        LOG_ERR("no device exists");
        return -1;
    }
    /* skip devices with zeroed node_guid (e.g. rdmaP* virtual device on Azure MANA) */
    ctx->ctx = NULL;
    for (int i = 0; dev_list[i]; i++) {
        struct ibv_device_attr dev_attr;
        struct ibv_context *tmp = ibv_open_device(dev_list[i]);
        if (!tmp) continue;
        if (ibv_query_device(tmp, &dev_attr) == 0 && dev_attr.node_guid != 0) {
            ctx->ctx = tmp;
            break;
        }
        ibv_close_device(tmp);
    }
    ibv_free_device_list(dev_list);

    if (ctx->ctx == NULL) {
        LOG_ERR("failed to open device");
        return -1;
    }

    ctx->pd = ibv_alloc_pd(ctx->ctx);
    if (ctx->pd == NULL) {
        LOG_ERR("failed to allocate context protection domain");
        return -1;
    }

    ctx->cq = ibv_create_cq(ctx->ctx, CQ_DEPTH, NULL, NULL, 0);
    if (ctx->cq == NULL) {
        LOG_ERR("failed to create context completion queue");
        return -1;
    }

    ctx->port = port;
    ctx->gid_index = gid_index;
    return 0;
}

void rdma_ctx_destroy(rdma_ctx_t *ctx) {
    if (ctx == NULL) {
        LOG_ERR("rdma context is null");
        return;
    }
    if (ctx->cq != NULL && ibv_destroy_cq(ctx->cq) != 0) {
        LOG_ERR("failed to destroy context completion queue");
    }
    if (ctx->pd != NULL && ibv_dealloc_pd (ctx->pd) != 0) {
        LOG_ERR("failed to deallocate protection domain");
    }
    if (ctx->ctx != NULL && ibv_close_device (ctx->ctx) != 0) {
        LOG_ERR("failed to release device");
    }
    memset(ctx, 0, sizeof(*ctx));
}