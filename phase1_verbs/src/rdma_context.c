#include "rdma_common.h"
#include "logging.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define CQ_DEPTH 128

/* Scan GID table via sysfs and return the first RoCE v2 GID index with a non-zero address.
 * Falls back to the caller-supplied default if none is found. */
static int find_roce_v2_gid(struct ibv_context *ctx, int port, int fallback) {
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx, port, &port_attr) != 0)
        return fallback;

    const char *dev_name = ibv_get_device_name(ctx->device);
    for (int i = 0; i < port_attr.gid_tbl_len; i++) {
        char path[256], type_str[32];
        snprintf(path, sizeof(path),
                 "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%d",
                 dev_name, port, i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int ok = fgets(type_str, sizeof(type_str), f) != NULL;
        fclose(f);
        if (!ok || strstr(type_str, "RoCE v2") == NULL)
            continue;

        union ibv_gid gid;
        if (ibv_query_gid(ctx, port, i, &gid) != 0)
            continue;
        if (gid.global.subnet_prefix || gid.global.interface_id)
            return i;
    }
    return fallback;
}

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
    ctx->gid_index = find_roce_v2_gid(ctx->ctx, port, gid_index);
    LOG_INFO("using device gid_index=%d", ctx->gid_index);
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