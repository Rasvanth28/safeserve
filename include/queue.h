#ifndef SAFESERVE_QUEUE_H
#define SAFESERVE_QUEUE_H

#include <stdbool.h>
#include <time.h>
#include <pthread.h>

typedef struct {
    int client_fd;
    struct timespec t_accept;
} job_t;

typedef struct {
    job_t *buf;
    int capacity;
    int head;
    int tail;
    int count;
    bool closed;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} queue_t;

int queue_init(queue_t *q, int capacity);
void queue_destroy(queue_t *q);

// Non-blocking push. Returns true if queued, false if queue is full or closed.
// FR-8: when full, reject immediately with 503.
bool queue_try_push(queue_t *q, job_t job);

// Blocking pop. Returns true if job retrieved, false if queue was closed and is empty.
bool queue_pop(queue_t *q, job_t *job);

// Marks queue as closed and wakes all waiting consumers.
void queue_close(queue_t *q);

int queue_count(queue_t *q);

#endif // SAFESERVE_QUEUE_H
