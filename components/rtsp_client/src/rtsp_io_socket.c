#include "rtsp_io_socket.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"

// io_ctx は rtsp_client_t が保持する int (ソケットfd、未接続時は-1) へのポインタ

static int sock_connect(void *io_ctx, const char *host, uint16_t port, uint32_t timeout_ms) {
    int *fd_out = (int *)io_ctx;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ret = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret != 0) {
        close(fd);
        return -1;
    }
    *fd_out = fd;
    return 0;
}

static int sock_send(void *io_ctx, const uint8_t *buf, size_t len) {
    int fd = *(int *)io_ctx;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

static int sock_recv(void *io_ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms) {
    int fd = *(int *)io_ctx;
    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t n = recv(fd, buf, cap, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        return -1; // 相手からの切断 (EOF)
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0; // timeout (recv自体は失敗していない)
    }
    return -1;
}

static void sock_close(void *io_ctx) {
    int *fd = (int *)io_ctx;
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

const rtsp_io_ops_t rtsp_io_socket_ops = {
    .connect = sock_connect,
    .send = sock_send,
    .recv = sock_recv,
    .close = sock_close,
};
