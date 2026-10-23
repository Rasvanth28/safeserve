#define _POSIX_C_SOURCE 200809L
#include "rm.h"
#include "banker.h"
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define NUM_THREADS 16
#define ITERATIONS_PER_THREAD 50

static resource_config_t test_rc = {
    .m = 5,
    .res_names = {"WORKER", "BUF", "FD", "DB", "CLOCK"},
    .total = {16, 64, 128, 4, 2},
    .num_routes = 0
};

static atomic_int completed_iterations = 0;
static atomic_bool invariant_failed = false;

extern rm_t g_rm;

static void verify_invariants(void) {
    pthread_mutex_lock(&g_rm.lock);
    for (int j = 0; j < g_rm.m; j++) {
        int sum_alloc = 0;
        for (int i = 0; i < N_MAX; i++) {
            if (g_rm.active[i]) {
                if (g_rm.alloc[i][j] > g_rm.max[i][j]) {
                    printf("Invariant violation: alloc > max for slot %d, res %d\n", i, j);
                    atomic_store(&invariant_failed, true);
                }
                sum_alloc += g_rm.alloc[i][j];
            }
        }

        if (g_rm.available[j] < 0 || g_rm.available[j] > g_rm.total[j]) {
            printf("Invariant violation: available out of bounds for res %d: %d\n", j, g_rm.available[j]);
            atomic_store(&invariant_failed, true);
        }

        if (g_rm.available[j] + sum_alloc != g_rm.total[j]) {
            printf("Invariant violation: available (%d) + sum_alloc (%d) != total (%d) for res %d\n",
                   g_rm.available[j], sum_alloc, g_rm.total[j], j);
            atomic_store(&invariant_failed, true);
        }
    }
    pthread_mutex_unlock(&g_rm.lock);
}

static void *stress_thread_fn(void *arg) {
    (void)arg;

    // Route claims: either DB-heavy or CLOCK-heavy
    int claim_a[M_MAX] = {1, 4, 2, 1, 0}; // Query-like: needs DB
    int claim_b[M_MAX] = {1, 8, 4, 0, 1}; // Upload-like: needs CLOCK
    int claim_c[M_MAX] = {1, 2, 2, 1, 1}; // Refresh-like: needs DB + CLOCK

    for (int iter = 0; iter < ITERATIONS_PER_THREAD; iter++) {
        int slot = rm_alloc_slot();
        assert(slot >= 0);

        int *claim = (iter % 3 == 0) ? claim_a : ((iter % 3 == 1) ? claim_b : claim_c);
        rm_register_claim(slot, claim);

        int rc = rm_acquire_atomic(slot, claim, 2000);
        if (rc == RM_SUCCESS) {
            // Check system invariants while holding resources
            verify_invariants();

            // Simulate small work
            struct timespec ts = {0, 200000}; // 200 us
            nanosleep(&ts, NULL);

            rm_release_all(slot);
        }

        rm_free_slot(slot);
        atomic_fetch_add(&completed_iterations, 1);
    }

    return NULL;
}

int main(void) {
    printf("=== Running Resource Manager & Banker's Stress Test ===\n");
    assert(rm_init(&test_rc, MODE_BANKER, ADMIT_ATOMIC, 2000) == 0);

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, stress_thread_fn, NULL);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    int total_expected = NUM_THREADS * ITERATIONS_PER_THREAD;
    int total_actual = atomic_load(&completed_iterations);
    bool failed = atomic_load(&invariant_failed);

    printf("[test_rm_stress] Completed %d / %d iterations under concurrent stress.\n",
           total_actual, total_expected);
    printf("[test_rm_stress] Invariants intact (available + alloc == total): %s\n",
           !failed ? "PASS" : "FAIL");

    assert(total_actual == total_expected);
    assert(!failed);

    rm_destroy();
    printf("=== All RM Stress Tests Passed! ===\n");
    return 0;
}
