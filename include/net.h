#ifndef SAFESERVE_NET_H
#define SAFESERVE_NET_H

#include <stdbool.h>

// Creates, binds, and sets socket to listen on the specified port.
// Returns socket file descriptor, or -1 on error.
int net_listen(int port, int backlog);

// Configures receive timeout on a client socket to guard against slowloris stalls.
int net_set_recv_timeout(int sock_fd, int timeout_sec);

// Closes listening socket
void net_close(int sock_fd);

#endif // SAFESERVE_NET_H
