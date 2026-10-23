#define _POSIX_C_SOURCE 200809L
#include "rm.h"
#include "banker.h"
#include "naive.h"
#include "lockorder.h"
#include "detect.h"
#include "metrics.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

rm_t g_rm;

int rm_init(const resource_config_t *rc, server_mode_t mode, int admit_timeout_ms, int detect_interval_ms) {
    if (!rc || rc->m <= 0 || rc->m > M_MAX) return -1;
    memset(&g_rm, 0, sizeof(g_rm));

    g_rm.m = rc->m;
    g_rm.mode = mode;
    g_rm.admit_timeout_ms = admit_timeout_ms > 0 ? admit_timeout_ms : 2000;
    g_rm.next_ticket = 1;
    g_rm.next_wait_ticket = 1;
    g_rm.aging_ms = 250;

    for (int j = 0; j < rc->m; j++) {
        g_rm.total[j] = rc->total[j];
        g_rm.available[j] = rc->total[j];
        strncpy(g_rm.res_names[j], rc->res_names[j], sizeof(g_rm.res_names[j]) - 1);
    }

    pthread_mutex_init(&g_rm.lock, NULL);
    pthread_cond_init(&g_rm.cv, NULL);

    if (mode == MODE_DETECT && detect_start(detect_interval_ms) != 0) {
        LOG_E("Failed to start deadlock detector thread");
        return -1;
    }
    return 0;
}

void rm_destroy(void) {
    rm_shutdown();
    if (g_rm.mode == MODE_DETECT) {
        detect_stop();
    }
    pthread_mutex_destroy(&g_rm.lock);
    pthread_cond_destroy(&g_rm.cv);
}

void rm_set_fairness_aging(int aging_ms) {
    pthread_mutex_lock(&g_rm.lock);
    g_rm.aging_ms = aging_ms > 0 ? aging_ms : 0;
    pthread_mutex_unlock(&g_rm.lock);
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
            g_rm.waiting[i] = false;
            for (int j = 0; j < M_MAX; j++) {
                g_rm.max[i][j] = 0;
                g_rm.alloc[i][j] = 0;
                g_rm.request[i][j] = 0;
            }
            g_rm.n_active++;
            pthread_mutex_unlock(&g_rm.lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_rm.lock);
    return -1;
}

// Caller holds g_rm.lock.
static void release_all_locked(int slot) {
    bool freed = false;
    for (int j = 0; j < g_rm.m; j++) {
        if (g_rm.alloc[slot][j] > 0) {
            g_rm.available[j] += g_rm.alloc[slot][j];
            g_rm.alloc[slot][j] = 0;
            freed = true;
        }
    }
    if (freed) pthread_cond_broadcast(&g_rm.cv);
}

void rm_free_slot(int slot) {
    if (slot < 0 || slot >= N_MAX) return;

    pthread_mutex_lock(&g_rm.lock);
    if (g_rm.active[slot]) {
        release_all_locked(slot);
        for (int j = 0; j < M_MAX; j++) {
            g_rm.max[slot][j] = 0;
        }
        g_rm.active[slot] = false;
        g_rm.aborted[slot] = false;
        g_rm.n_active--;
        // A finished claim can turn an unsafe grant into a safe one.
        pthread_cond_broadcast(&g_rm.cv);
    }
    pthread_mutex_unlock(&g_rm.lock);
}

int rm_register_claim(int slot, const int claim[M_MAX]) {
    if (slot < 0 || slot >= N_MAX) return RM_ERROR;

    pthread_mutex_lock(&g_rm.lock);
    for (int j = 0; j < g_rm.m; j++) {
        if (claim[j] < 0 || claim[j] > g_rm.total[j]) {
            pthread_mutex_unlock(&g_rm.lock);
            return RM_ERROR;
        }
    }
    for (int j = 0; j < g_rm.m; j++) {
        g_rm.max[slot][j] = claim[j];
    }
    pthread_mutex_unlock(&g_rm.lock);
    return RM_SUCCESS;
}

// Caller holds g_rm.lock. Pure "may this request be granted right now?"
static bool can_grant_locked(int slot, const int *req) {
    switch (g_rm.mode) {
        case MODE_BANKER: {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            bool ok = banker_can_grant(g_rm.available,
                                       (const int (*)[M_MAX])g_rm.alloc,
                                       (const int (*)[M_MAX])g_rm.max,
                                       g_rm.active, N_MAX, g_rm.m,
                                       slot, req);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            uint64_t dur_ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                              (uint64_t)(t1.tv_nsec - t0.tv_nsec);
            metrics_record_safety_check(dur_ns);
            g_rm.safety_checks++;
            if (!ok && naive_can_grant(g_rm.available, req, g_rm.m)) {
                g_rm.unsafe_denials++;
            }
            return ok;
        }
        case MODE_NAIVE:
        case MODE_LOCKORDER:
        case MODE_DETECT:
            return naive_can_grant(g_rm.available, req, g_rm.m);
    }
    return false;
}

// Caller holds g_rm.lock.
static bool holds_nothing_locked(int slot) {
    for (int j = 0; j < g_rm.m; j++) {
        if (g_rm.alloc[slot][j] > 0) return false;
    }
    return true;
}

// Caller holds g_rm.lock. Banker fairness (FR-17): should `slot` yield to an
// older waiter? Two rules:
//  1. Aging: once some waiter has waited aging_ms, slots that hold nothing
//     yet and queued after it (new requests) stay out of the way, so
//     resources drain to it. Slots already holding resources are never held
//     back by aging; they must finish to free what the starved waiter needs.
//  2. FIFO: an older waiter whose request could be granted right now goes
//     first - but only if rule 1 is not holding that waiter back, otherwise
//     the two would yield to each other and nobody would proceed.
// The oldest waiter that rule 1 does not hold back never yields, so some
// waiter always makes progress.
static bool should_yield_locked(int slot) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Wait ticket of the oldest aged waiter (UINT64_MAX if none).
    uint64_t oldest_aged = UINT64_MAX;
    if (g_rm.aging_ms > 0) {
        for (int i = 0; i < N_MAX; i++) {
            if (!g_rm.active[i] || !g_rm.waiting[i] || g_rm.aborted[i]) continue;
            if (g_rm.wait_ticket[i] < oldest_aged &&
                timespec_diff_ms(&g_rm.wait_since[i], &now) >= g_rm.aging_ms) {
                oldest_aged = g_rm.wait_ticket[i];
            }
        }
    }

    if (holds_nothing_locked(slot) && oldest_aged < g_rm.wait_ticket[slot]) {
        return true;
    }

    for (int i = 0; i < N_MAX; i++) {
        if (i == slot || !g_rm.active[i] || !g_rm.waiting[i] || g_rm.aborted[i]) continue;
        if (g_rm.wait_ticket[i] >= g_rm.wait_ticket[slot]) continue;
        if (holds_nothing_locked(i) && oldest_aged < g_rm.wait_ticket[i]) continue;

        if (banker_can_grant(g_rm.available,
                             (const int (*)[M_MAX])g_rm.alloc,
                             (const int (*)[M_MAX])g_rm.max,
                             g_rm.active, N_MAX, g_rm.m,
                             i, g_rm.request[i])) {
            return true;
        }
    }
    return false;
}

int rm_acquire_vec(int slot, const int req[M_MAX], int timeout_ms) {
    if (slot < 0 || slot >= N_MAX || !req) return RM_ERROR;

    bool any = false;
    for (int j = 0; j < g_rm.m; j++) {
        if (req[j] < 0) return RM_ERROR;
        if (req[j] > 0) any = true;
    }
    if (!any) return RM_SUCCESS;

    pthread_mutex_lock(&g_rm.lock);

    if (!g_rm.active[slot]) {
        pthread_mutex_unlock(&g_rm.lock);
        return RM_ERROR;
    }

    // A request beyond the declared claim is a handler bug in every mode.
    for (int j = 0; j < g_rm.m; j++) {
        if (g_rm.alloc[slot][j] + req[j] > g_rm.max[slot][j]) {
            pthread_mutex_unlock(&g_rm.lock);
            LOG_W("Slot %d requested %d %s beyond its claim of %d",
                  slot, req[j], g_rm.res_names[j], g_rm.max[slot][j]);
            return RM_ERROR;
        }
    }

    if (g_rm.mode == MODE_LOCKORDER && !lockorder_order_ok(g_rm.alloc[slot], req, g_rm.m)) {
        g_rm.order_violations++;
        pthread_mutex_unlock(&g_rm.lock);
        LOG_W("Slot %d violated the global lock order", slot);
        return RM_ORDER;
    }

    // Naive and detect wait without a deadline: a naive deadlock must stay a
    // deadlock, and in detect mode only the detector may break it.
    bool timed = (g_rm.mode == MODE_BANKER || g_rm.mode == MODE_LOCKORDER);
    if (timeout_ms <= 0) timeout_ms = g_rm.admit_timeout_ms;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    timespec_add_ms(&deadline, timeout_ms);

    for (int j = 0; j < g_rm.m; j++) {
        g_rm.request[slot][j] = req[j];
    }
    g_rm.waiting[slot] = true;
    g_rm.wait_ticket[slot] = g_rm.next_wait_ticket++;
    clock_gettime(CLOCK_MONOTONIC, &g_rm.wait_since[slot]);
    g_rm.n_waiting++;

    int rc;
    while (true) {
        if (g_rm.shutdown) {
            rc = RM_SHUTDOWN;
            break;
        }
        if (g_rm.aborted[slot]) {
            rc = RM_ABORTED;
            break;
        }
        if (can_grant_locked(slot, req) &&
            (g_rm.mode != MODE_BANKER || !should_yield_locked(slot))) {
            for (int j = 0; j < g_rm.m; j++) {
                g_rm.available[j] -= req[j];
                g_rm.alloc[slot][j] += req[j];
            }
            rc = RM_SUCCESS;
            break;
        }

        if (timed) {
            if (pthread_cond_timedwait(&g_rm.cv, &g_rm.lock, &deadline) == ETIMEDOUT) {
                rc = RM_TIMEOUT;
                break;
            }
        } else {
            pthread_cond_wait(&g_rm.cv, &g_rm.lock);
        }
    }

    for (int j = 0; j < g_rm.m; j++) {
        g_rm.request[slot][j] = 0;
    }
    g_rm.waiting[slot] = false;
    g_rm.n_waiting--;
    // Younger waiters that yielded to this one under the FIFO rule must
    // re-evaluate once it leaves the queue, whether or not it was granted.
    if (rc != RM_SUCCESS || g_rm.n_waiting > 0) pthread_cond_broadcast(&g_rm.cv);

    pthread_mutex_unlock(&g_rm.lock);

    if (rc == RM_TIMEOUT) metrics_inc_admission_timeout();
    return rc;
}

int rm_acquire(int slot, int res_idx, int count, int timeout_ms) {
    if (res_idx < 0 || res_idx >= g_rm.m || count <= 0) return RM_ERROR;
    int req[M_MAX] = {0};
    req[res_idx] = count;
    return rm_acquire_vec(slot, req, timeout_ms);
}

int rm_release(int slot, int res_idx, int count) {
    if (slot < 0 || slot >= N_MAX || res_idx < 0 || res_idx >= g_rm.m || count <= 0) {
        return RM_ERROR;
    }

    int rc = RM_ERROR;
    pthread_mutex_lock(&g_rm.lock);
    if (g_rm.alloc[slot][res_idx] >= count) {
        g_rm.alloc[slot][res_idx] -= count;
        g_rm.available[res_idx] += count;
        pthread_cond_broadcast(&g_rm.cv);
        rc = RM_SUCCESS;
    }
    pthread_mutex_unlock(&g_rm.lock);
    return rc;
}

void rm_release_all(int slot) {
    if (slot < 0 || slot >= N_MAX) return;
    pthread_mutex_lock(&g_rm.lock);
    release_all_locked(slot);
    pthread_mutex_unlock(&g_rm.lock);
}

int rm_detect_and_recover(void) {
    bool deadlocked[N_MAX];
    int victims = 0;

    pthread_mutex_lock(&g_rm.lock);
    while (true) {
        int count = detect_find_deadlocked(g_rm.available,
                                           (const int (*)[M_MAX])g_rm.alloc,
                                           (const int (*)[M_MAX])g_rm.request,
                                           g_rm.active, N_MAX, g_rm.m,
                                           deadlocked);
        if (count == 0) break;

        int victim = detect_pick_victim((const int (*)[M_MAX])g_rm.alloc,
                                        g_rm.ticket, deadlocked, N_MAX, g_rm.m);
        if (victim < 0) break;

        if (victims == 0) g_rm.deadlocks_found++;
        victims++;
        g_rm.victims_aborted++;
        LOG_W("Deadlock detected (%d requests); aborting victim slot %d", count, victim);

        // The victim is blocked in rm_acquire_vec, so it is not using what it
        // holds. Take it back, then wake it to return RM_ABORTED.
        g_rm.aborted[victim] = true;
        release_all_locked(victim);
        pthread_cond_broadcast(&g_rm.cv);
    }
    pthread_mutex_unlock(&g_rm.lock);
    return victims;
}

void rm_get_need(int slot, int need_out[M_MAX]) {
    for (int j = 0; j < M_MAX; j++) need_out[j] = 0;
    if (slot < 0 || slot >= N_MAX) return;
    pthread_mutex_lock(&g_rm.lock);
    for (int j = 0; j < g_rm.m; j++) {
        need_out[j] = g_rm.max[slot][j] - g_rm.alloc[slot][j];
    }
    pthread_mutex_unlock(&g_rm.lock);
}

void rm_get_snapshot(rm_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_rm.lock);
    out->m = g_rm.m;
    for (int j = 0; j < g_rm.m; j++) {
        memcpy(out->res_names[j], g_rm.res_names[j], sizeof(out->res_names[j]));
        out->total[j] = g_rm.total[j];
        out->available[j] = g_rm.available[j];
    }
    out->active_slots = g_rm.n_active;
    out->waiting_slots = g_rm.n_waiting;
    out->safety_checks = g_rm.safety_checks;
    out->unsafe_denials = g_rm.unsafe_denials;
    out->deadlocks_found = g_rm.deadlocks_found;
    out->victims_aborted = g_rm.victims_aborted;
    out->order_violations = g_rm.order_violations;
    pthread_mutex_unlock(&g_rm.lock);
}

const char *rm_strerror(int rc) {
    switch (rc) {
        case RM_SUCCESS:  return "granted";
        case RM_TIMEOUT:  return "admission timeout";
        case RM_ERROR:    return "claim violation";
        case RM_SHUTDOWN: return "server shutting down";
        case RM_ABORTED:  return "aborted as deadlock victim";
        case RM_ORDER:    return "lock-order violation";
    }
    return "unknown";
}
