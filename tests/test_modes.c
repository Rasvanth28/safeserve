#define _POSIX_C_SOURCE 200809L
// Behavioural test of the four strategies on the classic circular-wait
// workload: thread A takes DB then CLOCK, thread B takes CLOCK then DB, with a
// pause between the two acquisitions. One DB and one CLOCK exist.
//   naive     must deadlock (both stuck until shutdown)
//   banker    must complete both (B's first grant is refused as unsafe)
//   lockorder must complete both (B's plan is reordered to DB, CLOCK)
//   detect    must abort exactly one victim and complete the other
#include "rm.h"
#include "lockorder.h"
#include "config.h"
#include "util.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define DB 0
#define CLOCK 1

extern rm_t g_rm;

static resource_config_t rc2 = {
    .m = 2,
    .res_names = {"DB", "CLOCK"},
    .total = {1, 1},
};

typedef struct {
    int first, second;
    server_mode_t mode;
    int rc;
    atomic_bool done;
} actor_t;

static void *actor_fn(void *arg) {
    actor_t *a = arg;
    int slot = rm_alloc_slot();
    int claim[M_MAX] = {1, 1};
    assert(rm_register_claim(slot, claim) == RM_SUCCESS);

    acq_step_t plan[2] = { {a->first, 1}, {a->second, 1} };
    if (a->mode == MODE_LOCKORDER) lockorder_sort_plan(plan, 2);

    a->rc = rm_acquire(slot, plan[0].res, 1, 3000);
    if (a->rc == RM_SUCCESS) {
        sleep_ms(100); // Window for the other thread to take its first resource
        a->rc = rm_acquire(slot, plan[1].res, 1, 3000);
    }
    if (a->rc == RM_SUCCESS) sleep_ms(20);  // Work while holding both

    rm_free_slot(slot);
    atomic_store(&a->done, true);
    return NULL;
}

static void run_pair(server_mode_t mode, actor_t *a, actor_t *b, int watch_ms) {
    assert(rm_init(&rc2, mode, 3000, 50) == 0);
    *a = (actor_t){ .first = DB, .second = CLOCK, .mode = mode };
    *b = (actor_t){ .first = CLOCK, .second = DB, .mode = mode };
    atomic_init(&a->done, false);
    atomic_init(&b->done, false);

    pthread_t ta, tb;
    pthread_create(&ta, NULL, actor_fn, a);
    sleep_ms(10);
    pthread_create(&tb, NULL, actor_fn, b);

    for (int waited = 0; waited < watch_ms; waited += 10) {
        if (atomic_load(&a->done) && atomic_load(&b->done)) break;
        sleep_ms(10);
    }
    if (!atomic_load(&a->done) || !atomic_load(&b->done)) {
        rm_shutdown(); // Break a deadlock so the test can clean up
    }
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
}

static void test_naive_deadlocks(void) {
    actor_t a, b;
    run_pair(MODE_NAIVE, &a, &b, 1000);
    bool deadlocked = a.rc == RM_SHUTDOWN && b.rc == RM_SHUTDOWN;
    printf("[test_modes] naive: circular wait deadlocks (both blocked until shutdown): %s\n",
           deadlocked ? "PASS" : "FAIL");
    assert(deadlocked);
    rm_destroy();
}

static void test_banker_avoids(void) {
    actor_t a, b;
    run_pair(MODE_BANKER, &a, &b, 2000);
    rm_snapshot_t s;
    rm_get_snapshot(&s);
    printf("[test_modes] banker: both complete, %llu unsafe grant(s) refused: %s\n",
           (unsigned long long)s.unsafe_denials,
           (a.rc == RM_SUCCESS && b.rc == RM_SUCCESS && s.unsafe_denials > 0) ? "PASS" : "FAIL");
    assert(a.rc == RM_SUCCESS && b.rc == RM_SUCCESS);
    assert(s.unsafe_denials > 0);
    rm_destroy();
}

static void test_lockorder_prevents(void) {
    actor_t a, b;
    run_pair(MODE_LOCKORDER, &a, &b, 2000);
    assert(a.rc == RM_SUCCESS && b.rc == RM_SUCCESS);

    // Going downhill directly is refused.
    int slot = rm_alloc_slot();
    int claim[M_MAX] = {1, 1};
    rm_register_claim(slot, claim);
    assert(rm_acquire(slot, CLOCK, 1, 100) == RM_SUCCESS);
    assert(rm_acquire(slot, DB, 1, 100) == RM_ORDER);
    rm_free_slot(slot);

    printf("[test_modes] lockorder: both complete, downhill request refused: PASS\n");
    rm_destroy();
}

static void test_detect_recovers(void) {
    actor_t a, b;
    run_pair(MODE_DETECT, &a, &b, 2000);
    rm_snapshot_t s;
    rm_get_snapshot(&s);
    int ok = (a.rc == RM_SUCCESS) + (b.rc == RM_SUCCESS);
    int aborted = (a.rc == RM_ABORTED) + (b.rc == RM_ABORTED);
    printf("[test_modes] detect: deadlock found, 1 victim aborted, 1 completes: %s\n",
           (ok == 1 && aborted == 1 && s.deadlocks_found == 1) ? "PASS" : "FAIL");
    assert(ok == 1 && aborted == 1);
    assert(s.deadlocks_found == 1 && s.victims_aborted == 1);
    rm_destroy();
}

static void test_claim_enforced(void) {
    assert(rm_init(&rc2, MODE_BANKER, 500, 50) == 0);
    int slot = rm_alloc_slot();
    int too_big[M_MAX] = {2, 0};
    assert(rm_register_claim(slot, too_big) == RM_ERROR);   // FR-15: claim > total
    int claim[M_MAX] = {1, 0};
    assert(rm_register_claim(slot, claim) == RM_SUCCESS);
    assert(rm_acquire(slot, CLOCK, 1, 100) == RM_ERROR);     // Beyond declared claim
    rm_free_slot(slot);
    rm_destroy();
    printf("[test_modes] Claims above capacity or beyond declaration rejected: PASS\n");
}

// ---- FR-17: a large claim is not starved by a stream of small ones ----

static resource_config_t rc_buf = { .m = 1, .res_names = {"BUF"}, .total = {4} };
#define SMALL_THREADS 12
static atomic_bool stop_small;

static void *small_fn(void *arg) {
    (void)arg;
    while (!atomic_load(&stop_small)) {
        int slot = rm_alloc_slot();
        int claim[M_MAX] = {1};
        rm_register_claim(slot, claim);
        if (rm_acquire(slot, 0, 1, 2000) == RM_SUCCESS) sleep_ms(5);
        rm_free_slot(slot);
    }
    return NULL;
}

static int big_wait(int aging_ms, double *waited_ms) {
    assert(rm_init(&rc_buf, MODE_BANKER, 1500, 50) == 0);
    rm_set_fairness_aging(aging_ms);
    atomic_store(&stop_small, false);

    pthread_t small[SMALL_THREADS];
    for (int i = 0; i < SMALL_THREADS; i++) pthread_create(&small[i], NULL, small_fn, NULL);
    sleep_ms(30);

    int slot = rm_alloc_slot();
    int claim[M_MAX] = {4};
    rm_register_claim(slot, claim);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = rm_acquire(slot, 0, 4, 1500);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *waited_ms = timespec_diff_ms(&t0, &t1);
    rm_free_slot(slot);

    atomic_store(&stop_small, true);
    for (int i = 0; i < SMALL_THREADS; i++) pthread_join(small[i], NULL);
    rm_destroy();
    return rc;
}

static void test_fairness(void) {
    double waited_off, waited_on;
    int rc_off = big_wait(0, &waited_off);
    int rc_on = big_wait(100, &waited_on);
    printf("[test_modes] Starvation: big claim without aging -> %s after %.0f ms (informational)\n",
           rm_strerror(rc_off), waited_off);
    // With aging the wait is bounded by roughly aging_ms + one hold time.
    bool bounded = rc_on == RM_SUCCESS && waited_on < 400.0;
    printf("[test_modes] Starvation: big claim with 100 ms aging -> %s after %.0f ms (bound 400 ms): %s\n",
           rm_strerror(rc_on), waited_on, bounded ? "PASS" : "FAIL");
    assert(bounded);
}

int main(void) {
    log_set_level(LOG_LEVEL_ERROR); // Victim/violation warnings are expected here
    printf("=== Running Strategy Behaviour Tests ===\n");
    test_naive_deadlocks();
    test_banker_avoids();
    test_lockorder_prevents();
    test_detect_recovers();
    test_claim_enforced();
    test_fairness();
    printf("=== All Strategy Behaviour Tests Passed! ===\n");
    return 0;
}
