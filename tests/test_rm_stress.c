#define _POSIX_C_SOURCE 200809L
// Randomized stress of the resource manager in incremental mode. Threads
// acquire their claim one resource at a time in a random order (sorted in
// lockorder mode) with random hold times. A checker thread asserts the
// invariants of EDD §11.4 throughout:
//   0 <= available[j] <= total[j]
//   available[j] + sum_i alloc[i][j] == total[j]
//   alloc[i][j] <= max[i][j]
//   banker mode: every committed state is safe
// Every mode except naive must finish all iterations (no hang past the watchdog).
#include "rm.h"
#include "banker.h"
#include "lockorder.h"
#include "config.h"
#include "util.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define NUM_THREADS 24
#define ITERATIONS_PER_THREAD 60
#define WATCHDOG_MS 30000

extern rm_t g_rm;

static resource_config_t test_rc = {
    .m = 5,
    .res_names = {"WORKER", "BUF", "FD", "DB", "CLOCK"},
    .total = {16, 32, 64, 3, 2},
};

static const int claims[4][M_MAX] = {
    {1, 8, 1, 1, 0},  // query
    {1, 16, 4, 1, 1}, // upload
    {1, 4, 2, 1, 1},  // refresh
    {1, 2, 2, 0, 0},  // static
};

static server_mode_t g_mode;
static atomic_int completed, granted, timeouts, aborted, errors;
static atomic_bool invariant_failed, stop_checker;

static void check_invariants(void) {
    pthread_mutex_lock(&g_rm.lock);
    for (int j = 0; j < g_rm.m; j++) {
        int sum = 0;
        for (int i = 0; i < N_MAX; i++) {
            if (!g_rm.active[i]) continue;
            if (g_rm.alloc[i][j] > g_rm.max[i][j]) atomic_store(&invariant_failed, true);
            sum += g_rm.alloc[i][j];
        }
        if (g_rm.available[j] < 0 || g_rm.available[j] > g_rm.total[j] ||
            g_rm.available[j] + sum != g_rm.total[j]) {
            atomic_store(&invariant_failed, true);
        }
    }
    if (g_mode == MODE_BANKER &&
        !banker_is_safe(g_rm.available, (const int (*)[M_MAX])g_rm.alloc,
                        (const int (*)[M_MAX])g_rm.max, g_rm.active, N_MAX, g_rm.m)) {
        atomic_store(&invariant_failed, true);
    }
    pthread_mutex_unlock(&g_rm.lock);
}

static void *checker_fn(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_checker)) {
        check_invariants();
        sleep_ms(1);
    }
    return NULL;
}

static void *stress_fn(void *arg) {
    unsigned int seed = (unsigned int)(uintptr_t)arg;

    for (int iter = 0; iter < ITERATIONS_PER_THREAD; iter++) {
        int slot = rm_alloc_slot();
        assert(slot >= 0);
        const int *claim = claims[rand_r(&seed) % 4];
        assert(rm_register_claim(slot, claim) == RM_SUCCESS);

        // Random acquisition order (Fisher-Yates over the claimed types)
        acq_step_t plan[M_MAX];
        int n = 0;
        for (int j = 0; j < test_rc.m; j++) {
            if (claim[j] > 0) plan[n++] = (acq_step_t){ j, claim[j] };
        }
        for (int k = n - 1; k > 0; k--) {
            int r = rand_r(&seed) % (k + 1);
            acq_step_t t = plan[k]; plan[k] = plan[r]; plan[r] = t;
        }
        if (g_mode == MODE_LOCKORDER) n = lockorder_sort_plan(plan, n);

        int rc = RM_SUCCESS;
        for (int s = 0; s < n && rc == RM_SUCCESS; s++) {
            rc = rm_acquire(slot, plan[s].res, plan[s].count, 2000);
            if (rc == RM_SUCCESS) sleep_ms(rand_r(&seed) % 2);
        }

        if (rc == RM_SUCCESS) { atomic_fetch_add(&granted, 1); sleep_ms(rand_r(&seed) % 3); }
        else if (rc == RM_TIMEOUT) atomic_fetch_add(&timeouts, 1);
        else if (rc == RM_ABORTED) atomic_fetch_add(&aborted, 1);
        else atomic_fetch_add(&errors, 1);

        rm_free_slot(slot);
        atomic_fetch_add(&completed, 1);
    }
    return NULL;
}

static void run_mode(server_mode_t mode, const char *name, int aging_ms) {
    g_mode = mode;
    atomic_store(&completed, 0);
    atomic_store(&granted, 0);
    atomic_store(&timeouts, 0);
    atomic_store(&aborted, 0);
    atomic_store(&errors, 0);
    atomic_store(&invariant_failed, false);
    atomic_store(&stop_checker, false);

    assert(rm_init(&test_rc, mode, 2000, 20) == 0);
    rm_set_fairness_aging(aging_ms);

    pthread_t checker, threads[NUM_THREADS];
    pthread_create(&checker, NULL, checker_fn, NULL);
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, stress_fn, (void *)(uintptr_t)(1000 + i));
    }

    int expected = NUM_THREADS * ITERATIONS_PER_THREAD;
    int waited = 0;
    while (atomic_load(&completed) < expected && waited < WATCHDOG_MS) {
        sleep_ms(20);
        waited += 20;
    }
    bool hung = atomic_load(&completed) < expected;
    if (hung) rm_shutdown();

    for (int i = 0; i < NUM_THREADS; i++) pthread_join(threads[i], NULL);
    atomic_store(&stop_checker, true);
    pthread_join(checker, NULL);

    rm_snapshot_t s;
    rm_get_snapshot(&s);
    printf("[test_rm_stress] %-10s granted=%d timeouts=%d victims=%d errors=%d hung=%s invariants=%s\n",
           name, atomic_load(&granted), atomic_load(&timeouts), atomic_load(&aborted),
           atomic_load(&errors), hung ? "yes" : "no",
           atomic_load(&invariant_failed) ? "FAIL" : "ok");

    assert(!hung);
    assert(!atomic_load(&invariant_failed));
    assert(atomic_load(&errors) == 0);
    if (mode != MODE_DETECT) assert(atomic_load(&aborted) == 0);
    rm_destroy();
}

int main(void) {
    log_set_level(LOG_LEVEL_ERROR); // Victim/violation warnings are expected here
    printf("=== Running Resource Manager Stress Test (incremental, random order) ===\n");
    run_mode(MODE_BANKER, "banker", 250);
    // Aggressive aging keeps the starvation guard engaged most of the time,
    // exercising its interplay with the FIFO rule (no livelock allowed).
    run_mode(MODE_BANKER, "banker/20", 20);
    run_mode(MODE_LOCKORDER, "lockorder", 250);
    run_mode(MODE_DETECT, "detect", 250);
    printf("=== All RM Stress Tests Passed! ===\n");
    return 0;
}
