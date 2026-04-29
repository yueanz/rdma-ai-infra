#include "rdma_backend.hpp"
#include <cstdlib>
#include <cstring>
#include <unistd.h>

RdmaTransport::RdmaTransport() {
    memset(&qp_, 0, sizeof(qp_));
}

RdmaTransport::~RdmaTransport() {
    close();
}

int RdmaTransport::reg_buf(void *buf, size_t size, BufferHandle *out) {
    rai_mr_t *mr;
    if (buf == nullptr || out == nullptr || size == 0) {
        LOG_ERR("rdma reg_buf failed: buf is nullptr or out is nullptr or size is 0");
        return -1;
    }

    mr = new rai_mr_t{};
    if (mr == nullptr) {
        LOG_ERR("rdma reg_buf failed: mr new failed");
        return -1;
    }

    if (rai_mr_reg_external(&qp_, mr, buf, size) != 0) {
        LOG_ERR("rdma reg_buf failed: rai_mr_reg failed");
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
    rai_mr_t *mr = (rai_mr_t*)h->priv;
    rai_mr_dereg(mr);
    delete(mr);
    h->priv = nullptr;
}

int RdmaTransport::send_async(const BufferHandle *h, size_t len, uint64_t id, size_t offset) {
    if (h == nullptr) {
        LOG_ERR("rdma send_async failed: h is nullptr");
        return -1;
    }
    if (rai_post_send(&qp_, (rai_mr_t*)h->priv, len, id, offset) != 0) {
        LOG_ERR("rdma send_async failed: rai_post_send failed");
        return -1;
    }
    return 0;
}

int RdmaTransport::recv_async(BufferHandle *h, size_t len, uint64_t id, size_t offset) {
    if (h == nullptr) {
        LOG_ERR("rdma recv_async failed: h is nullptr");
        return -1;
    }
    if (rai_post_recv(&qp_, (rai_mr_t*)h->priv, len, id, offset) != 0) {
        LOG_ERR("rdma recv_async failed: rai_post_recv failed");
        return -1;
    }
    return 0;
}

int RdmaTransport::exchange_buf(const BufferHandle *local, uint64_t *remote_addr,
                            uint32_t *rkey) {
    rai_mr_t *mr;
    if (local == nullptr || local->priv == nullptr) {
        LOG_ERR("rdma exchange_buf failed: local or local->priv is nullptr");
        return -1;
    }
    if (remote_addr == NULL || rkey == NULL) {
        LOG_ERR("rdma exchange_buf failed: remote_addr or rkey is nullptr");
        return -1;
    }
    mr = (rai_mr_t*)local->priv;
    if (mr->mr == nullptr) {
        LOG_ERR("rdma exchange_buf failed: mr->mr is nullptr");
        return -1;
    }

    qp_.local.addr = (uint64_t)(uintptr_t)mr->mr->addr;
    qp_.local.rkey = mr->mr->rkey;

    if (is_server_) {
        if (rai_oob_accept(mr_listen_fd_, &qp_) != 0) {
            LOG_ERR("rdma exchange_buf failed: rdma_accept failed");
            return -1;
        }
    } else {
        if (host_.empty() || rai_oob_connect(&qp_, host_.c_str(), port_+1) != 0) {
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
    if (rai_post_write(&qp_, (rai_mr_t*)local->priv, len, IBV_SEND_SIGNALED,
                            remote_addr + offset, rkey, id, offset) != 0) {
        LOG_ERR("rdma write_async failed: rai_post_write failed");
        return -1;
    }
    return 0;
}

int RdmaTransport::read_async(const BufferHandle *local, uint64_t remote_addr,
                            uint32_t rkey, size_t len, uint64_t id, size_t offset) {
    if (local == nullptr) {
        LOG_ERR("rdma read_async failed: local is nullptr");
        return -1;
    }
    if (rai_post_read(&qp_, (rai_mr_t*)local->priv, len,
                            remote_addr + offset, rkey, id, offset) != 0) {
        LOG_ERR("rdma read_async failed: rai_post_read failed");
        return -1;
    }
    return 0;
}

int RdmaTransport::poll(uint64_t *completed_id) {
    return rai_poll_cq(&qp_, completed_id);
}

int RdmaTransport::connect(const char *host, int port) {
    if (host == nullptr) {
        LOG_ERR("rdma connect failed: host is nullptr");
        return -1;
    }

    if (qp_.qp != nullptr) {
        rai_qp_destroy(&qp_);
    }

    /* Defensive: if this instance was previously used as a listener,
     * its OOB mr_listen_fd_ will still be open. */
    if (mr_listen_fd_ >= 0) {
        ::close(mr_listen_fd_);
        mr_listen_fd_ = -1;
    }

    if (rai_cm_connect_qp(&qp_, host, port) != 0) {
        LOG_ERR("rdma connect failed: rai_cm_connect_qp failed");
        return -1;
    }

    is_server_ = false;
    host_ = host;
    port_ = port;
    return 0;
}

int RdmaTransport::listen(int port) {
    if (qp_.qp != nullptr) {
        rai_qp_destroy(&qp_);
    }

    if (mr_listen_fd_ >= 0) {
        ::close(mr_listen_fd_);
        mr_listen_fd_ = -1;
    }

    if (rai_cm_listen_qp(&qp_, port, &mr_listen_fd_) != 0) {
        LOG_ERR("rdma listen failed: rai_cm_listen_qp failed");
        return -1;
    }
    
    is_server_ = true;
    return 0;
}

int RdmaTransport::accept() {
    if (rai_cm_accept_qp(&qp_) != 0) {
        LOG_ERR("rdma accept failed: rai_cm_accept_qp failed");
        return -1;
    }
    return 0;
}

void RdmaTransport::close() {
    rai_qp_destroy(&qp_);
    if (mr_listen_fd_ >= 0) {
        ::close(mr_listen_fd_);
        mr_listen_fd_ = -1;
    }
}

Transport *create_rdma_transport() { return new RdmaTransport(); }