#include <rdma/rdma_cma.h>
#include "rdma_common.h"
#include "logging.h"
#include <string.h>

/* The QP belongs to the cm_id, and ctx is borrowed from it — neither is
 * ours to destroy directly. Order: QP → CQ → PD → cm_id → ec. */
static void qp_destroy_cm(rai_qp_t *qp) {
    struct rdma_cm_id *id = (struct rdma_cm_id *)qp->cm_id;
    if (id && id->qp)
        rdma_destroy_qp(id);
    if (qp->cq && ibv_destroy_cq(qp->cq) != 0)
        LOG_ERR("ibv_destroy_cq failed");
    if (qp->pd && ibv_dealloc_pd(qp->pd) != 0)
        LOG_ERR("ibv_dealloc_pd failed");
    if (id)
        rdma_destroy_id(id);
    if (qp->ec)
        rdma_destroy_event_channel((struct rdma_event_channel *)qp->ec);
}

/* We created the QP and opened the device, so we tear both down.
 * Order: QP → CQ → PD → device. */
static void qp_destroy_verbs(rai_qp_t *qp) {
    if (qp->qp && ibv_destroy_qp(qp->qp) != 0)
        LOG_ERR("ibv_destroy_qp failed");
    if (qp->cq && ibv_destroy_cq(qp->cq) != 0)
        LOG_ERR("ibv_destroy_cq failed");
    if (qp->pd && ibv_dealloc_pd(qp->pd) != 0)
        LOG_ERR("ibv_dealloc_pd failed");
    if (qp->ctx && ibv_close_device(qp->ctx) != 0)
        LOG_ERR("ibv_close_device failed");
}

/* Caller must rai_mr_dereg() before calling this (MR depends on PD).
 * Idempotent: safe on a partially-initialized qp (any field may be NULL). */
void rai_qp_destroy(rai_qp_t *qp) {
    if (qp == NULL)
        return;
    if (qp->mode == RAI_CONN_VERBS)
        qp_destroy_verbs(qp);
    else
        qp_destroy_cm(qp);
    memset(qp, 0, sizeof(*qp));
}
