#include "rdma_backend.hpp"
#include <cstdlib>
#include <unistd.h>

RdmaTransport::RdmaTransport() {
    memset(&ctx_, 0, sizeof(ctx_));
    memset(&qp_, 0, sizeof(qp_));
}

RdmaTransport::~RdmaTransport() {
    close();
}

int RdmaTransport::reg_buf(void *buf, size_t size, BufferHandle *out) {
    rdma_mr_t *mr;
    if (buf == nullptr || out == nullptr || size == 0) {
        LOG_ERR("rdma reg_buf failed: buf is nullptr or out is nullptr or size is 0");
        return -1;
    }

    mr = new rdma_mr_t{};
    if (mr == nullptr) {
        LOG_ERR("rdma reg_buf failed: mr new failed");
        return -1;
    }

    if (rdma_mr_reg_external(&ctx_, mr, buf, size) != 0) {
        LOG_ERR("rdma reg_buf failed: rdma_mr_reg failed");
        delete(mr);
        return -1;
    }
    out->addr = buf;
    out->size = size;
    out->priv = mr;
    return 0;
}

void RdmaTransport::dereg_buf(BufferHandle *h) {
    if (h == nullptr)
        return;
    h->addr = nullptr;
    h->size = 0;
    rdma_mr_t *mr = (rdma_mr_t*)h->priv;
    rdma_mr_dereg(mr);
    delete(mr);
    h->priv = nullptr;
}

int RdmaTransport::send_async(const BufferHandle *h, size_t len, uint64_t id, size_t offset) {
    if (h == nullptr) {
        LOG_ERR("rdma send_async failed: h is nullptr");
        return -1;
    }
    if (rdma_post_send(&qp_, (rdma_mr_t*)h->priv, len, id, offset) != 0) {
        LOG_ERR("rdma send_async failed: rdma_post_send failed");
        return -1;
    }
    return 0;
}

int RdmaTransport::recv_async(BufferHandle *h, size_t len, uint64_t id, size_t offset) {
    if (h == nullptr) {
        LOG_ERR("rdma recv_async failed: h is nullptr");
        return -1;
    }
    if (rdma_post_recv(&qp_, (rdma_mr_t*)h->priv, len, id, offset) != 0) {
        LOG_ERR("rdma recv_async failed: rdma_post_recv failed");
        return -1;
    }
    return 0;
}

int RdmaTransport::exchange_buf(const BufferHandle *local, uint64_t *remote_addr,
                            uint32_t *rkey) {
    rdma_mr_t *mr;
    if (local == nullptr || local->priv == nullptr) {
        LOG_ERR("rdma exchange_buf failed: local or local->priv is nullptr");
        return -1;
    }
    if (remote_addr == NULL || rkey == NULL) {
        LOG_ERR("rdma exchange_buf failed: remote_addr or rkey is nullptr");
        return -1;
    }
    mr = (rdma_mr_t*)local->priv;
    if (mr->mr == nullptr) {
        LOG_ERR("rdma exchange_buf failed: mr->mr is nullptr");
        return -1;
    }

    qp_.local.addr = (uint64_t)(uintptr_t)mr->mr->addr;
    qp_.local.rkey = mr->mr->rkey;

    if (is_server_) {
        if (rdma_accept(listen_fd_, &qp_) != 0) {
            LOG_ERR("rdma exchange_buf failed: rdma_accept failed");
            return -1;
        }
    } else {
        if (host_.empty() || rdma_exchange_info_client(&qp_, host_.c_str(), port_) != 0) {
            LOG_ERR("rdma exchange_buf failed: rdma_exchange_info_client failed");
            return -1;
        }
    }
    *remote_addr = qp_.remote.addr;
    *rkey = qp_.remote.rkey;

    return 0;
}

int RdmaTransport::write_async(const BufferHandle *local, uint64_t remote_addr,
                            uint32_t rkey, size_t len, uint64_t id, size_t offset) {
    if (local == nullptr) {
        LOG_ERR("rdma write_async failed: local is nullptr");
        return -1;
    }
    if (rdma_post_write(&qp_, (rdma_mr_t*)local->priv, len, IBV_SEND_SIGNALED,
                            remote_addr + offset, rkey, id, offset) != 0) {
        LOG_ERR("rdma write_async failed: rdma_post_write failed");
        return -1;
    }
    return 0;
}

int RdmaTransport::poll(uint64_t *completed_id) {
    return rdma_poll_cq(&ctx_, completed_id);
}

int RdmaTransport::connect(const char *host, int port) {
    if (host == nullptr) {
        LOG_ERR("rdma connect failed: host is nullptr");
        return -1;
    }

    if (qp_.qp != nullptr) {
        rdma_qp_destroy(&qp_);
    }

    if (ctx_.ctx != nullptr) {
        rdma_ctx_destroy(&ctx_);
    }

    if (rdma_ctx_init(&ctx_, 1, 1) != 0) {
        LOG_ERR("rdma connect failed: rdma_ctx_init failed");
        return -1;
    }

    if (rdma_qp_create(&ctx_, &qp_) != 0) {
        LOG_ERR("rdma connect failed: rdma_qp_create failed");
        return -1;
    }

    if (rdma_qp_init(&ctx_, &qp_) != 0) {
        LOG_ERR("rdma connect failed: rdma_qp_init failed");
        return -1;
    }

    if (rdma_exchange_info_client(&qp_, host, port) != 0) {
        LOG_ERR("rdma connect failed: rdma_exchange_info_client failed");
        return -1;
    }

    if (rdma_qp_connect(&ctx_, &qp_) != 0) {
        LOG_ERR("rdma connect failed: rdma_qp_connect failed");
        return -1;
    }

    is_server_ = false;
    host_ = host;
    port_ = port;
    return 0;
}

int RdmaTransport::listen(int port) {
    if (qp_.qp != nullptr) {
        rdma_qp_destroy(&qp_);
    }

    if (ctx_.ctx != nullptr) {
        rdma_ctx_destroy(&ctx_);
    }

    if (rdma_ctx_init(&ctx_, 1, 1) != 0) {
        LOG_ERR("rdma listen failed: rdma_ctx_init failed");
        return -1;
    }

    if (rdma_qp_create(&ctx_, &qp_) != 0) {
        LOG_ERR("rdma listen failed: rdma_qp_create failed");
        return -1;
    }

    if (rdma_qp_init(&ctx_, &qp_) != 0) {
        LOG_ERR("rdma listen failed: rdma_qp_init failed");
        return -1;
    }

    if (rdma_listen(port, &listen_fd_) != 0) {
        LOG_ERR("rdma listen failed: rdma_listen failed");
        return -1;
    }
    is_server_ = true;
    return 0;
}

int RdmaTransport::accept() {
    if (rdma_accept(listen_fd_, &qp_) != 0) {
        LOG_ERR("rdma accept failed: rdma_accept failed");
        return -1;
    }
    if (rdma_qp_connect(&ctx_, &qp_) != 0) {
        LOG_ERR("rdma accept failed: rdma_qp_connect failed");
        return -1;
    }
    return 0;
}

void RdmaTransport::close() {
    rdma_qp_destroy(&qp_);
    rdma_ctx_destroy(&ctx_);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

Transport *create_rdma_transport() { return new RdmaTransport(); }