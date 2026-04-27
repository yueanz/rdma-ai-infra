#include <rdma/rdma_cma.h>
#include "rdma_common.h"
#include "logging.h"
#include <string.h>

/* Destroy order matters: QP → CQ → PD → cm_id → ec.
 * Caller must rai_mr_dereg() before calling this (MR depends on PD). */
void rai_qp_destroy(rai_qp_t *qp) {
    if (qp == NULL)
        return;
    struct rdma_cm_id *id = (struct rdma_cm_id *)qp->cm_id;
    if (id && id->qp)
        rdma_destroy_qp(id);
    if (qp->cq)
        ibv_destroy_cq(qp->cq);
    if (qp->pd)
        ibv_dealloc_pd(qp->pd);
    if (id)
        rdma_destroy_id(id);
    if (qp->ec)
        rdma_destroy_event_channel((struct rdma_event_channel *)qp->ec);
    memset(qp, 0, sizeof(*qp));
}
