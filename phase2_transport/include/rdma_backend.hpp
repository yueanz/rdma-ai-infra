#pragma once
#include "transport.hpp"
#include "rdma_common.h"
#include <string>

class RdmaTransport : public Transport {
public:
    RdmaTransport();
    ~RdmaTransport() override;
    RdmaTransport(const RdmaTransport&) = delete;
    RdmaTransport& operator=(const RdmaTransport&) = delete;

    int reg_buf(void *buf, size_t size, BufferHandle *out) override;
    void dereg_buf(BufferHandle *h) override;

    int send_async(const BufferHandle *h, size_t len, uint64_t id, size_t offset) override;
    int recv_async(BufferHandle *h, size_t len, uint64_t id, size_t offset) override;
    int write_async(const BufferHandle *local, uint64_t remote_addr, uint32_t rkey,
                            size_t len, uint64_t id, size_t offset) override;
    int read_async(const BufferHandle *local, uint64_t remote_addr, uint32_t rkey,
                            size_t len, uint64_t id, size_t offset) override;
    int exchange_buf(const BufferHandle *local, uint64_t *remote_addr,
                            uint32_t *rkey) override;

    int poll(uint64_t *completed_id) override;

    int connect(const char *host, int port) override;
    int listen(int port) override;
    int accept() override;
    void close() override;

private:
    rai_qp_t qp_;
    int port_ = -1;
    int mr_listen_fd_ = -1;
    bool is_server_ = false;
    std::string host_;
};


