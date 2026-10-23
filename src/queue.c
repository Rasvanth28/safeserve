#define _POSIX_C_SOURCE 200809L
#include "queue.h"

#include <stdlib.h>
#include <string.h>

int queue_init(queue_t *q, int capacity) {
    if (!q || capacity <= 0) return -1;
    memset(q, 0, sizeof(*q));

    q->buf = malloc(sizeof(job_t) * capacity);
    if (!q->buf) return -1;

    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->closed = false;

    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);

    return 0;
}

void queue_destroy(queue_t *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);

    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);

    if (q->buf) {
        free(q->buf);
        q->buf = NULL;
    }
}

bool queue_try_push(queue_t *q, job_t job) {
    if (!q) return false;

    pthread_mutex_lock(&q->mu);
    if (q->closed || q->count == q->capacity) {
        pthread_mutex_unlock(&q->mu);
        return false;
    }

    q->buf[q->tail] = job;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return true;
}

bool queue_pop(queue_t *q, job_t *job) {
    if (!q || !job) return false;

    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }

    if (q->count == 0 && q->closed) {
        pthread_mutex_unlock(&q->mu);
        return false;
    }

    *job = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return true;
}

void queue_close(queue_t *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

int queue_count(queue_t *q) {
    if (!q) return 0;
    pthread_mutex_lock(&q->mu);
    int cnt = q->count;
    pthread_mutex_unlock(&q->mu);
    return cnt;
}
