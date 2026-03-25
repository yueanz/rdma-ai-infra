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

int rdma_exchange_info_server(rdma_qp_t *qp, int port) {
    int server_fd = -1, client_fd = -1, opt = 1, ret = -1;
    struct sockaddr_in addr = {0};

    if (qp == NULL) {
        LOG_ERR("rdma queue pair is null");
        return -1;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERR("socket failed");
        goto out;
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        LOG_INFO("setsockopt failed");

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind failed");
        goto out;
    }
    if (listen(server_fd, 1) < 0) {
        LOG_ERR("listen failed");
        goto out;
    }

    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        LOG_ERR("accept failed");
        goto out;
    }

    if (send_all(client_fd, &qp->local, sizeof(rdma_conn_info_t)) != 0) {
        LOG_ERR("send_all failed");
        goto out;
    }

    if (recv_all(client_fd, &qp->remote, sizeof(rdma_conn_info_t)) != 0) {
        LOG_ERR("recv_all failed");
        goto out;
    }

    ret = 0;
out:
    if (server_fd >= 0)
        close(server_fd);
    if (client_fd >= 0)
        close(client_fd);
    return ret;
}

int rdma_exchange_info_client(rdma_qp_t *qp, const char *server_ip, int port) {
    int server_fd = -1, ret = -1;
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

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERR("socket failed");
        goto out;
    }

    if (connect(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("connect failed");
        goto out;
    }

    if (recv_all(server_fd, &qp->remote, sizeof(rdma_conn_info_t)) != 0) {
        LOG_ERR("recv_all failed");
        goto out;
    }

    if (send_all(server_fd, &qp->local, sizeof(rdma_conn_info_t)) != 0) {
        LOG_ERR("send_all failed");
        goto out;
    }

    ret = 0;
out:
    if (server_fd >= 0)
        close(server_fd);
    return ret;
}