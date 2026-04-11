#include "tcp_backend.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int send_all(int fd, const void *buf, size_t len) {
    ssize_t n;
    size_t sent = 0;
    while (sent < len) {
        n = send(fd, (char*)buf + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    ssize_t n;
    size_t recved = 0;
    while (recved < len) {
        n = recv(fd, (char*)buf + recved, len - recved, 0);
        if (n <= 0) {
            return -1;
        }
        recved += n;
    }
    return 0;
}

TcpTransport::TcpTransport() : fd_(-1), listen_fd_(-1), is_server_(false) {}

TcpTransport::~TcpTransport() {
    close();
}

int TcpTransport::reg_buf(void *buf, size_t size, BufferHandle *out) {
    if (buf == nullptr || out == nullptr || size == 0) {
        LOG_ERR("tcp reg_buf failed: buf is null or out is null or size is 0");
        return -1;
    }
    out->addr = buf;
    out->size = size;
    out->priv = nullptr;
    return 0;
}

void TcpTransport::dereg_buf(BufferHandle *h) {
    if (h == nullptr)
        return;
    h->addr = nullptr;
    h->size = 0;
    h->priv = nullptr;
}

int TcpTransport::send_async(const BufferHandle *h, size_t len, uint64_t id, size_t offset) {
    if (h == nullptr || len == 0) {
        LOG_ERR("tcp send_async failed: h is null or len is 0");
        return -1;
    }
    if (len > h->size) {
        LOG_ERR("tcp send_async failed: len exceeds the size of buf");
        return -1;
    }
    if (h->addr == nullptr) {
        LOG_ERR("tcp send_async failed: buf is null");
        return -1;
    }

    if (send_all(fd_, (char*)h->addr + offset, len) != 0) {
        LOG_ERR("tcp send_async failed: send_all failed");
        return -1;
    }

    return 0;
}

int TcpTransport::recv_async(BufferHandle *h, size_t len, uint64_t id, size_t offset) {
    if (h == nullptr || len == 0) {
        LOG_ERR("tcp recv_async failed: h is null or len is 0");
        return -1;
    }
    if (len > h->size) {
        LOG_ERR("tcp recv_async failed: len exceeds the size of buf");
        return -1;
    }
    if (h->addr == nullptr) {
        LOG_ERR("tcp recv_async failed: buf is null");
        return -1;
    }

    if (recv_all(fd_, (char*)h->addr + offset, len) != 0) {
        LOG_ERR("tcp recv_async failed: recv_all failed");
        return -1;
    }

    return 0;
}

int TcpTransport::exchange_buf(const BufferHandle *local, uint64_t *remote_addr,
                            uint32_t *rkey) {
    if (local == nullptr || remote_addr == nullptr || rkey == nullptr) {
        LOG_ERR("tcp exchange_buf failed: local or remote_addr or rkey is nullptr");
        return -1;
    }
    uint64_t local_addr = (uint64_t)(uintptr_t)local->addr;

    if (is_server_) {
        if (send_all(fd_, &local_addr, sizeof(local_addr)) != 0) {
            LOG_ERR("tcp exchange_buf failed: send_all failed");
            return -1;
        }
        if (recv_all(fd_, remote_addr, sizeof(*remote_addr)) != 0) {
            LOG_ERR("tcp exchange_buf failed: recv_all failed");
            return -1;
        }
    } else {
        if (recv_all(fd_, remote_addr, sizeof(*remote_addr)) != 0) {
            LOG_ERR("tcp exchange_buf failed: recv_all failed");
            return -1;
        }
        if (send_all(fd_, &local_addr, sizeof(local_addr)) != 0) {
            LOG_ERR("tcp exchange_buf failed: send_all failed");
            return -1;
        }
    }
    *rkey = 0;
    return 0;
}

int TcpTransport::write_async(const BufferHandle *local, uint64_t remote_addr,
                            uint32_t rkey, size_t len, uint64_t id, size_t offset) {
    return send_async(local, len, id, offset);
}

int TcpTransport::poll(uint64_t *completed_id) {
    return 0;
}

int TcpTransport::connect(const char *host, int port) {
    struct sockaddr_in addr = {0};

    if (host == nullptr) {
        LOG_ERR("tcp connect failed: host is null");
        return -1;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        LOG_ERR("tcp connect failed: invalid server_ip: %s", host);
        return -1;
    }

    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        LOG_ERR("tcp connect failed: socket failed");
        return -1;
    }

    if (::connect(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("tcp connect failed: connect failed");
        return -1;
    }
    is_server_ = false;
    return 0;
}
int TcpTransport::listen(int port) {
    int opt = 1;
    struct sockaddr_in addr = {0};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERR("tcp listen failed: socket failed");
        return -1;
    }
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        LOG_INFO("setsockopt failed");

    if (bind(listen_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("tcp listen failed: bind failed");
        return -1;
    }

    if (::listen(listen_fd_, 1) < 0) {
        LOG_ERR("tcp listen failed: listen failed");
        return -1;
    }
    is_server_ = true;
    return 0;
}
int TcpTransport::accept() {
    fd_ = ::accept(listen_fd_, NULL, NULL);
    if (fd_ < 0) {
        LOG_ERR("tcp accept failed: accept failed");
        return -1;
    }
    return 0;
}
void TcpTransport::close() {
    if (fd_ >= 0)
        ::close(fd_);
    if (listen_fd_ >= 0)
        ::close(listen_fd_);
}

Transport *create_tcp_transport() { return new TcpTransport(); }