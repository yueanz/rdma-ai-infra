#include <infiniband/verbs.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "rdma_common.h"
#include "logging.h"

#define CQ_DEPTH 128

/* ::ffff:a.b.c.d — the GID a RoCE port gets for an IPv4 address on its netdev */
static int gid_is_ipv4_mapped(const union ibv_gid *gid) {
    static const uint8_t v4_prefix[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff };
    return memcmp(gid->raw, v4_prefix, sizeof(v4_prefix)) == 0;
}

static int gid_is_link_local(const union ibv_gid *gid) {
    return gid->raw[0] == 0xfe && (gid->raw[1] & 0xc0) == 0x80;
}

/* This path is RoCE-only: the RTR transition below always routes via GRH.
 *
 * A port typically carries several RoCE v2 GIDs — one per address on its
 * netdev, including the auto-generated IPv6 link-local one. Taking the first
 * would pick that link-local entry, while rdma_cm resolves an IPv4 peer to the
 * IPv4-mapped GID, so the two connection modes would not even use the same IP
 * version. Prefer IPv4-mapped, then any routable GID, and only fall back to
 * link-local if the port has nothing else. */
static int find_roce_v2_gid(struct ibv_context *ctx, int port) {
    struct ibv_port_attr port_attr;
    int routable = -1, link_local = -1;

    if (ibv_query_port(ctx, port, &port_attr) != 0) {
        LOG_ERR("ibv_query_port failed");
        return -1;
    }
    for (int i = 0; i < port_attr.gid_tbl_len; i++) {
        struct ibv_gid_entry entry;
        if (ibv_query_gid_ex(ctx, port, i, &entry, 0) != 0)
            continue;   /* empty table slot */
        if (entry.gid_type != IBV_GID_TYPE_ROCE_V2)
            continue;
        if (gid_is_ipv4_mapped(&entry.gid))
            return i;
        if (gid_is_link_local(&entry.gid)) {
            if (link_local < 0)
                link_local = i;
        } else if (routable < 0) {
            routable = i;
        }
    }
    if (routable >= 0)
        return routable;
    if (link_local >= 0)
        LOG_INFO("no IPv4 RoCE v2 GID on port %d, falling back to link-local index %d",
                 port, link_local);
    return link_local;
}

static struct ibv_context *open_device(void) {
    struct ibv_device **list = ibv_get_device_list(NULL);
    struct ibv_context *ctx = NULL;
    const char *want = getenv("RDMA_DEVICE");

    if (list == NULL) {
        LOG_ERR("ibv_get_device_list failed");
        return NULL;
    }
    for (int i = 0; list[i]; i++) {
        struct ibv_device_attr dev_attr;
        struct ibv_context *tmp;

        if (want && strcmp(ibv_get_device_name(list[i]), want) != 0)
            continue;
        tmp = ibv_open_device(list[i]);
        if (tmp == NULL)
            continue;
        /* A zero node_guid means a placeholder entry, not a usable HCA. */
        if (!want && ibv_query_device(tmp, &dev_attr) == 0 && dev_attr.node_guid == 0) {
            ibv_close_device(tmp);
            continue;
        }
        ctx = tmp;
        break;
    }
    ibv_free_device_list(list);
    if (ctx == NULL)
        LOG_ERR("no usable RDMA device (set RDMA_DEVICE to select one)");
    return ctx;
}

/* Open the device and build pd + cq + qp, leaving the QP in INIT and
 * qp->local filled with what the peer needs for its RTR transition.
 * On failure the caller must rai_qp_destroy(qp). */
static int build_qp_verbs(rai_qp_t *qp) {
    struct ibv_qp_init_attr init_attr = {0};
    struct ibv_qp_attr attr = {0};

    qp->ctx = open_device();
    if (qp->ctx == NULL)
        return -1;

    if (qp->port_num == 0)
        qp->port_num = 1;

    qp->gid_index = find_roce_v2_gid(qp->ctx, qp->port_num);
    if (qp->gid_index < 0) {
        LOG_ERR("no RoCE v2 GID on port %d", qp->port_num);
        return -1;
    }

    qp->pd = ibv_alloc_pd(qp->ctx);
    if (qp->pd == NULL) {
        LOG_ERR("ibv_alloc_pd failed");
        return -1;
    }

    qp->cq = ibv_create_cq(qp->ctx, CQ_DEPTH, NULL, NULL, 0);
    if (qp->cq == NULL) {
        LOG_ERR("ibv_create_cq failed");
        return -1;
    }

    init_attr.send_cq = qp->cq;
    init_attr.recv_cq = qp->cq;
    init_attr.qp_type = IBV_QPT_RC;
    init_attr.cap.max_send_wr  = RAI_QP_MAX_WR;
    init_attr.cap.max_recv_wr  = RAI_QP_MAX_WR;
    init_attr.cap.max_send_sge = 1;
    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_inline_data = RAI_MAX_INLINE;
    qp->qp = ibv_create_qp(qp->pd, &init_attr);
    if (qp->qp == NULL) {
        LOG_ERR("ibv_create_qp failed");
        return -1;
    }
    qp->max_inline = init_attr.cap.max_inline_data;  /* updated to actual */

    qp->local.qpn = qp->qp->qp_num;
    qp->local.psn = 0;   /* both sides use 0, as NCCL does — nothing to sync */
    if (ibv_query_gid(qp->ctx, qp->port_num, qp->gid_index, &qp->local.gid) != 0) {
        LOG_ERR("ibv_query_gid failed");
        return -1;
    }

    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = qp->port_num;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(qp->qp, &attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        LOG_ERR("INIT failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* INIT → RTR → RTS. Requires qp->remote to be filled by the OOB exchange. */
static int qp_to_rts(rai_qp_t *qp) {
    struct ibv_port_attr port_attr;
    struct ibv_qp_attr attr = {0};

    if (qp->qp == NULL) {
        LOG_ERR("qp_to_rts failed: qp->qp is null");
        return -1;
    }
    if (ibv_query_port(qp->ctx, qp->port_num, &port_attr) != 0) {
        LOG_ERR("ibv_query_port failed");
        return -1;
    }

    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = port_attr.active_mtu;
    attr.dest_qp_num        = qp->remote.qpn;
    attr.rq_psn             = qp->remote.psn;
    attr.max_dest_rd_atomic = 16;  /* see rdma_cm_connect.c */
    /* 31 (~491ms per retry) matches what the CM path sets after rdma_accept,
     * so a CM-vs-verbs comparison isn't skewed by different RNR budgets. */
    attr.min_rnr_timer      = 12;   /* 0.64 ms; see rdma_cm_connect.c */
    attr.ah_attr.is_global      = 1;
    attr.ah_attr.grh.dgid       = qp->remote.gid;
    attr.ah_attr.grh.sgid_index = qp->gid_index;
    attr.ah_attr.grh.hop_limit  = 0xff;
    attr.ah_attr.port_num       = qp->port_num;
    if (ibv_modify_qp(qp->qp, &attr,
            IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
        LOG_ERR("INIT->RTR failed: %s", strerror(errno));
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.sq_psn        = qp->local.psn;
    attr.max_rd_atomic = 16;       /* see rdma_cm_connect.c */
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    if (ibv_modify_qp(qp->qp, &attr,
            IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC |
            IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY) != 0) {
        LOG_ERR("RTR->RTS failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int rai_verbs_listen_qp(rai_qp_t *qp, int port, int *mr_listen_fd) {
    int ep_fd = -1, ret = -1;

    qp->mode = RAI_CONN_VERBS;
    if (mr_listen_fd != NULL)
        *mr_listen_fd = -1;

    /* Both listeners go up before anything slow happens, so a client that
     * starts at the same time can never outrun either one. `port` carries
     * the qpn/psn/gid exchange, completed here because RTR needs it;
     * `port + 1` carries the MR addr/rkey the caller exchanges later, once
     * it has a PD to register against. */
    if (rai_oob_listen(port, &ep_fd) != 0) {
        LOG_ERR("rai_verbs_listen_qp failed: rai_oob_listen failed");
        goto out;
    }
    if (mr_listen_fd != NULL && rai_oob_listen(port + 1, mr_listen_fd) != 0) {
        LOG_ERR("rai_verbs_listen_qp failed: rai_oob_listen (mr) failed");
        goto out;
    }
    if (build_qp_verbs(qp) != 0) {
        LOG_ERR("rai_verbs_listen_qp failed: build_qp_verbs failed");
        goto out;
    }
    LOG_INFO("waiting for verbs connection on port %d", port);
    if (rai_oob_accept(ep_fd, qp) != 0) {
        LOG_ERR("rai_verbs_listen_qp failed: rai_oob_accept failed");
        goto out;
    }
    ret = 0;
out:
    if (ep_fd >= 0)
        close(ep_fd);
    if (ret != 0) {
        if (mr_listen_fd != NULL && *mr_listen_fd >= 0) {
            close(*mr_listen_fd);
            *mr_listen_fd = -1;
        }
        rai_qp_destroy(qp);
    }
    return ret;
}

int rai_verbs_accept_qp(rai_qp_t *qp) {
    if (qp_to_rts(qp) != 0) {
        LOG_ERR("rai_verbs_accept_qp failed: qp_to_rts failed");
        return -1;
    }
    return 0;
}

int rai_verbs_connect_qp(rai_qp_t *qp, const char *server_ip, int port) {
    int ret = -1;

    qp->mode = RAI_CONN_VERBS;

    if (build_qp_verbs(qp) != 0) {
        LOG_ERR("rai_verbs_connect_qp failed: build_qp_verbs failed");
        goto out;
    }
    if (rai_oob_connect(qp, server_ip, port) != 0) {
        LOG_ERR("rai_verbs_connect_qp failed: rai_oob_connect failed");
        goto out;
    }
    if (qp_to_rts(qp) != 0) {
        LOG_ERR("rai_verbs_connect_qp failed: qp_to_rts failed");
        goto out;
    }
    ret = 0;
out:
    if (ret != 0)
        rai_qp_destroy(qp);
    return ret;
}
