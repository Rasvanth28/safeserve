#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int net_listen(int port, int backlog) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_E("Failed to create socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_W("setsockopt SO_REUSEADDR failed");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("Failed to bind socket to port %d", port);
        close(sock_fd);
        return -1;
    }

    if (listen(sock_fd, backlog) < 0) {
        LOG_E("Failed to listen on socket");
        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

int net_set_recv_timeout(int sock_fd, int timeout_sec) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    return setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void net_close(int sock_fd) {
    if (sock_fd >= 0) {
        close(sock_fd);
    }
}
