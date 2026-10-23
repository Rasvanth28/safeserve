#define _POSIX_C_SOURCE 200809L
#include "lockorder.h"
#include <pthread.h>
#include <string.h>

static int lo_avail[M_MAX];
static pthread_mutex_t lo_mu[M_MAX];
static pthread_cond_t  lo_cv[M_MAX];
static int lo_m = 0;

int lockorder_init(const resource_config_t *rc) {
    if (!rc) return -1;
    lo_m = rc->m;
    for (int i = 0; i < lo_m; i++) {
        lo_avail[i] = rc->total[i];
        pthread_mutex_init(&lo_mu[i], NULL);
        pthread_cond_init(&lo_cv[i], NULL);
    }
    return 0;
}

void lockorder_destroy(void) {
    for (int i = 0; i < lo_m; i++) {
        pthread_mutex_destroy(&lo_mu[i]);
        pthread_cond_destroy(&lo_cv[i]);
    }
    lo_m = 0;
}

int lockorder_acquire_atomic(const int claim[M_MAX]) {
    // Acquire strictly in ascending resource index order (0 to lo_m - 1)
    for (int i = 0; i < lo_m; i++) {
        int needed = claim[i];
        if (needed <= 0) continue;

        pthread_mutex_lock(&lo_mu[i]);
        while (lo_avail[i] < needed) {
            pthread_cond_wait(&lo_cv[i], &lo_mu[i]);
        }
        lo_avail[i] -= needed;
        pthread_mutex_unlock(&lo_mu[i]);
    }
    return 0;
}

void lockorder_release_all(const int claim[M_MAX]) {
    // Release in reverse or arbitrary order
    for (int i = lo_m - 1; i >= 0; i--) {
        int count = claim[i];
        if (count <= 0) continue;

        pthread_mutex_lock(&lo_mu[i]);
        lo_avail[i] += count;
        pthread_cond_broadcast(&lo_cv[i]);
        pthread_mutex_unlock(&lo_mu[i]);
    }
}
