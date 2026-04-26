#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "logging.h"
#include "rdma_common.h"

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

int rai_oob_listen(int port, int *listen_fd) {
    int fd = -1, opt = 1;
    struct sockaddr_in addr = {0};

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR("rdma_listen failed: socket failed");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        LOG_INFO("rdma_listen failed: setsockopt failed");

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("rdma_listen failed: bind failed");
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        LOG_ERR("rdma_listen failed: listen failed");
        close(fd);
        return -1;
    }

    *listen_fd = fd;
    return 0;
}

int rai_oob_accept(int listen_fd, rai_qp_t *qp) {
    int conn_fd = -1;

    if (qp == NULL) {
        LOG_ERR("rdma_accept failed: qp is null");
        return -1;
    }

    conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        LOG_ERR("rdma_accept failed: accept failed");
        return -1;
    }

    if (send_all(conn_fd, &qp->local, sizeof(qp->local)) != 0) {
        LOG_ERR("rdma_accept failed: send_all failed");
        close(conn_fd);
        return -1;
    }

    if (recv_all(conn_fd, &qp->remote, sizeof(qp->remote)) != 0) {
        LOG_ERR("rdma_accept failed: recv_all failed");
        close(conn_fd);
        return -1;
    }

    close(conn_fd);
    return 0;
}

int rai_oob_connect(rai_qp_t *qp, const char *server_ip, int port) {
    int conn_fd = -1, ret = -1;
    struct sockaddr_in addr = {0};
    if (qp == NULL) {
        LOG_ERR("rdma queue pair is null");
        return -1;
    }
    if (server_ip == NULL) {
        LOG_ERR("server_ip is null");
        return -1;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        LOG_ERR("invalid server_ip: %s", server_ip);
        return -1;
    }

    conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn_fd < 0) {
        LOG_ERR("socket failed");
        goto out;
    }

    if (connect(conn_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("connect failed");
        goto out;
    }

    if (recv_all(conn_fd, &qp->remote, sizeof(qp->remote)) != 0) {
        LOG_ERR("recv_all failed");
        goto out;
    }

    if (send_all(conn_fd, &qp->local, sizeof(qp->local)) != 0) {
        LOG_ERR("send_all failed");
        goto out;
    }

    ret = 0;
out:
    if (conn_fd >= 0)
        close(conn_fd);
    return ret;
}