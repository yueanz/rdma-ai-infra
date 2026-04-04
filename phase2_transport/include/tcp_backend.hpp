#pragma once
#include "transport.hpp"

class TcpTransport : public Transport {
public:
    TcpTransport();
    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    int reg_buf(void *buf, size_t size, BufferHandle *out) override;
    void dereg_buf(BufferHandle *h) override;

    int send_async(const BufferHandle *h, size_t len, uint64_t id) override;
    int recv_async(BufferHandle *h, size_t len, uint64_t id) override;
    int write_async(const BufferHandle *local, uint64_t remote_addr, uint32_t rkey,
                            size_t len, uint64_t id) override;
    int exchange_buf(const BufferHandle *local, uint64_t *remote_addr,
                            uint32_t *rkey) override;

    int poll(uint64_t *completed_id) override;

    int connect(const char *host, int port) override;
    int listen(int port) override;
    int accept() override;
    void close() override;

private:
    int fd_;
    int listen_fd_;
    bool is_server_;
};



