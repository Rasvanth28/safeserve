#define _POSIX_C_SOURCE 200809L
#include "rm.h"
#include "banker.h"
#include "naive.h"
#include "lockorder.h"
#include "detect.h"
#include "metrics.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

rm_t g_rm;

int rm_init(const resource_config_t *rc, server_mode_t mode, admit_mode_t admit_mode, int admit_timeout_ms) {
    if (!rc) return -1;
    memset(&g_rm, 0, sizeof(g_rm));

    g_rm.m = rc->m;
    g_rm.mode = mode;
    g_rm.admit_mode = admit_mode;
    g_rm.admit_timeout_ms = admit_timeout_ms > 0 ? admit_timeout_ms : 2000;
    g_rm.shutdown = false;
    g_rm.next_ticket = 1;

    for (int j = 0; j < rc->m; j++) {
        g_rm.total[j] = rc->total[j];
        g_rm.available[j] = rc->total[j];
        strncpy(g_rm.res_names[j], rc->res_names[j], sizeof(g_rm.res_names[j]) - 1);
    }

    pthread_mutex_init(&g_rm.lock, NULL);
    pthread_cond_init(&g_rm.cv, NULL);

    if (mode == MODE_NAIVE) {
        naive_init(rc);
    } else if (mode == MODE_LOCKORDER) {
        lockorder_init(rc);
    } else if (mode == MODE_DETECT) {
        detect_init(rc, 200);
        detect_start();
    }

    return 0;
}

void rm_destroy(void) {
    rm_shutdown();

    if (g_rm.mode == MODE_NAIVE) {
        naive_destroy();
    } else if (g_rm.mode == MODE_LOCKORDER) {
        lockorder_destroy();
    } else if (g_rm.mode == MODE_DETECT) {
        detect_destroy();
    }

    pthread_mutex_destroy(&g_rm.lock);
    pthread_cond_destroy(&g_rm.cv);
}

void rm_shutdown(void) {
    pthread_mutex_lock(&g_rm.lock);
    g_rm.shutdown = true;
    pthread_cond_broadcast(&g_rm.cv);
    pthread_mutex_unlock(&g_rm.lock);
}

int rm_alloc_slot(void) {
    pthread_mutex_lock(&g_rm.lock);
    for (int i = 0; i < N_MAX; i++) {
        if (!g_rm.active[i]) {
            g_rm.active[i] = true;
            g_rm.ticket[i] = g_rm.next_ticket++;
            g_rm.aborted[i] = false;
            for (int j = 0; j < M_MAX; j++) {
                g_rm.max[i][j] = 0;
                g_rm.alloc[i][j] = 0;
            }
            pthread_mutex_unlock(&g_rm.lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_rm.lock);
    return -1;
}

void rm_free_slot(int slot) {
    if (slot < 0 || slot >= N_MAX) return;

    pthread_mutex_lock(&g_rm.lock);
    if (g_rm.active[slot]) {
        for (int j = 0; j < g_rm.m; j++) {
            g_rm.available[j] += g_rm.alloc[slot][j];
            g_rm.alloc[slot][j] = 0;
            g_rm.max[slot][j] = 0;
        }
        g_rm.active[slot] = false;
        g_rm.aborted[slot] = false;
        pthread_cond_broadcast(&g_rm.cv);
    }
    pthread_mutex_unlock(&g_rm.lock);
}

int rm_register_claim(int slot, const int claim[M_MAX]) {
    if (slot < 0 || slot >= N_MAX) return -1;

    pthread_mutex_lock(&g_rm.lock);
    for (int j = 0; j < g_rm.m; j++) {
        g_rm.max[slot][j] = claim[j];
    }
    pthread_mutex_unlock(&g_rm.lock);
    return 0;
}

static void add_ms_to_timespec(struct timespec *ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

int rm_acquire_atomic(int slot, const int claim[M_MAX], int timeout_ms) {
    if (slot < 0 || slot >= N_MAX) return RM_ERROR;

    // Zero-claim routes (e.g. /health, /metrics) pass immediately
    bool all_zero = true;
    for (int j = 0; j < g_rm.m; j++) {
        if (claim[j] > 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) return RM_SUCCESS;

    if (timeout_ms <= 0) timeout_ms = g_rm.admit_timeout_ms;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    add_ms_to_timespec(&deadline, timeout_ms);

    if (g_rm.mode == MODE_LOCKORDER) {
        // Enforce lock-ordering prevention baseline
        lockorder_acquire_atomic(claim);
        pthread_mutex_lock(&g_rm.lock);
        for (int j = 0; j < g_rm.m; j++) {
            g_rm.available[j] -= claim[j];
            g_rm.alloc[slot][j] += claim[j];
        }
        pthread_mutex_unlock(&g_rm.lock);
        return RM_SUCCESS;
    }

    pthread_mutex_lock(&g_rm.lock);

    while (true) {
        if (g_rm.shutdown) {
            pthread_mutex_unlock(&g_rm.lock);
            return RM_SHUTDOWN;
        }
        if (g_rm.aborted[slot]) {
            pthread_mutex_unlock(&g_rm.lock);
            return RM_ERROR;
        }

        bool can_proceed = false;

        if (g_rm.mode == MODE_BANKER) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            can_proceed = banker_can_grant(g_rm.available,
                                           (const int (*)[M_MAX])g_rm.alloc,
                                           (const int (*)[M_MAX])g_rm.max,
                                           g_rm.active,
                                           N_MAX, g_rm.m,
                                           slot, claim);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            uint64_t dur_ns = (t1.tv_sec - t0.tv_sec) * 1000000000ULL + (t1.tv_nsec - t0.tv_nsec);
            metrics_record_safety_check(dur_ns);
        } else {
            // MODE_NAIVE or MODE_DETECT without pre-grant avoidance
            bool has_avail = true;
            for (int j = 0; j < g_rm.m; j++) {
                if (claim[j] > g_rm.available[j]) {
                    has_avail = false;
                    break;
                }
            }
            can_proceed = has_avail;
        }

        if (can_proceed) {
            for (int j = 0; j < g_rm.m; j++) {
                g_rm.available[j] -= claim[j];
                g_rm.alloc[slot][j] += claim[j];
            }
            pthread_mutex_unlock(&g_rm.lock);
            return RM_SUCCESS;
        }

        int rc = pthread_cond_timedwait(&g_rm.cv, &g_rm.lock, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&g_rm.lock);
            metrics_inc_admission_timeout();
            return RM_TIMEOUT;
        }
    }
}

int rm_acquire(int slot, int res_idx, int count, int timeout_ms) {
    if (slot < 0 || slot >= N_MAX || res_idx < 0 || res_idx >= g_rm.m || count <= 0) {
        return RM_ERROR;
    }

    if (g_rm.mode == MODE_NAIVE) {
        naive_acquire(res_idx, count);
        pthread_mutex_lock(&g_rm.lock);
        g_rm.alloc[slot][res_idx] += count;
        pthread_mutex_unlock(&g_rm.lock);
        return RM_SUCCESS;
    }

    int req[M_MAX] = {0};
    req[res_idx] = count;
    return rm_acquire_atomic(slot, req, timeout_ms);
}

int rm_release(int slot, int res_idx, int count) {
    if (slot < 0 || slot >= N_MAX || res_idx < 0 || res_idx >= g_rm.m || count <= 0) {
        return RM_ERROR;
    }

    pthread_mutex_lock(&g_rm.lock);
    if (g_rm.alloc[slot][res_idx] >= count) {
        g_rm.alloc[slot][res_idx] -= count;
        g_rm.available[res_idx] += count;
        pthread_cond_broadcast(&g_rm.cv);
    }
    pthread_mutex_unlock(&g_rm.lock);

    if (g_rm.mode == MODE_NAIVE) {
        naive_release(res_idx, count);
    }
    return RM_SUCCESS;
}

void rm_release_all(int slot) {
    if (slot < 0 || slot >= N_MAX) return;

    if (g_rm.mode == MODE_LOCKORDER) {
        pthread_mutex_lock(&g_rm.lock);
        int claim[M_MAX];
        for (int j = 0; j < g_rm.m; j++) {
            claim[j] = g_rm.alloc[slot][j];
            g_rm.available[j] += g_rm.alloc[slot][j];
            g_rm.alloc[slot][j] = 0;
            g_rm.max[slot][j] = 0;
        }
        pthread_mutex_unlock(&g_rm.lock);
        lockorder_release_all(claim);
        return;
    }

    pthread_mutex_lock(&g_rm.lock);
    if (g_rm.active[slot]) {
        for (int j = 0; j < g_rm.m; j++) {
            if (g_rm.alloc[slot][j] > 0) {
                if (g_rm.mode == MODE_NAIVE) {
                    naive_release(j, g_rm.alloc[slot][j]);
                }
                g_rm.available[j] += g_rm.alloc[slot][j];
                g_rm.alloc[slot][j] = 0;
            }
            g_rm.max[slot][j] = 0;
        }
        pthread_cond_broadcast(&g_rm.cv);
    }
    pthread_mutex_unlock(&g_rm.lock);
}

void rm_get_snapshot(int avail_out[M_MAX], int alloc_sum_out[M_MAX], int *active_slots) {
    pthread_mutex_lock(&g_rm.lock);
    int active_cnt = 0;
    for (int j = 0; j < g_rm.m; j++) {
        avail_out[j] = g_rm.available[j];
        alloc_sum_out[j] = 0;
    }
    for (int i = 0; i < N_MAX; i++) {
        if (g_rm.active[i]) {
            active_cnt++;
            for (int j = 0; j < g_rm.m; j++) {
                alloc_sum_out[j] += g_rm.alloc[i][j];
            }
        }
    }
    if (active_slots) *active_slots = active_cnt;
    pthread_mutex_unlock(&g_rm.lock);
}
