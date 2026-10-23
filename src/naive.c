#define _POSIX_C_SOURCE 200809L
#include "naive.h"
#include <pthread.h>
#include <string.h>

static int naive_avail[M_MAX];
static pthread_mutex_t naive_mu[M_MAX];
static pthread_cond_t  naive_cv[M_MAX];
static int naive_m = 0;

int naive_init(const resource_config_t *rc) {
    if (!rc) return -1;
    naive_m = rc->m;
    for (int i = 0; i < naive_m; i++) {
        naive_avail[i] = rc->total[i];
        pthread_mutex_init(&naive_mu[i], NULL);
        pthread_cond_init(&naive_cv[i], NULL);
    }
    return 0;
}

void naive_destroy(void) {
    for (int i = 0; i < naive_m; i++) {
        pthread_mutex_destroy(&naive_mu[i]);
        pthread_cond_destroy(&naive_cv[i]);
    }
    naive_m = 0;
}

int naive_acquire(int res_idx, int count) {
    if (res_idx < 0 || res_idx >= naive_m || count <= 0) return 0;

    pthread_mutex_lock(&naive_mu[res_idx]);
    while (naive_avail[res_idx] < count) {
        pthread_cond_wait(&naive_cv[res_idx], &naive_mu[res_idx]);
    }
    naive_avail[res_idx] -= count;
    pthread_mutex_unlock(&naive_mu[res_idx]);

    return 0;
}

void naive_release(int res_idx, int count) {
    if (res_idx < 0 || res_idx >= naive_m || count <= 0) return;

    pthread_mutex_lock(&naive_mu[res_idx]);
    naive_avail[res_idx] += count;
    pthread_cond_broadcast(&naive_cv[res_idx]);
    pthread_mutex_unlock(&naive_mu[res_idx]);
}
