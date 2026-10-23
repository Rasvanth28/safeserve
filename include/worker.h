#ifndef SAFESERVE_WORKER_H
#define SAFESERVE_WORKER_H

#include <pthread.h>
#include "config.h"
#include "queue.h"

typedef struct {
    server_config_t cfg;
    queue_t queue;
    int listen_fd;
    pthread_t *worker_threads;
    int num_workers;
} server_context_t;

int worker_pool_start(server_context_t *ctx);
void worker_pool_stop(server_context_t *ctx);

#endif // SAFESERVE_WORKER_H
