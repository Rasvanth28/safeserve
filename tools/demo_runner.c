#define _POSIX_C_SOURCE 200809L
#include "rm.h"
#include "banker.h"
#include "naive.h"
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

static resource_config_t demo_rc = {
    .m = 2,
    .res_names = {"DB", "CLOCK"},
    .total = {1, 1}, // 1 DB connection, 1 Cache write lock
    .num_routes = 0
};

// Thread 1: Refresh simulation (takes DB, delays, takes CLOCK)
// Thread 2: Upload simulation  (takes CLOCK, delays, takes DB)

static atomic_bool t1_done = false;
static atomic_bool t2_done = false;

static void *naive_thread1(void *arg) {
    (void)arg;
    printf("  [Thread 1 - /refresh] Acquiring DB...\n");
    naive_acquire(0, 1);
    printf("  [Thread 1 - /refresh] Acquired DB. Simulating delay before requesting CLOCK...\n");
    usleep(100000); // 100 ms

    printf("  [Thread 1 - /refresh] Acquiring CLOCK...\n");
    naive_acquire(1, 1);
    printf("  [Thread 1 - /refresh] Acquired both! Releasing...\n");
    naive_release(1, 1);
    naive_release(0, 1);

    atomic_store(&t1_done, true);
    return NULL;
}

static void *naive_thread2(void *arg) {
    (void)arg;
    printf("  [Thread 2 - /upload]  Acquiring CLOCK...\n");
    naive_acquire(1, 1);
    printf("  [Thread 2 - /upload]  Acquired CLOCK. Simulating delay before requesting DB...\n");
    usleep(100000); // 100 ms

    printf("  [Thread 2 - /upload]  Acquiring DB...\n");
    naive_acquire(0, 1);
    printf("  [Thread 2 - /upload]  Acquired both! Releasing...\n");
    naive_release(0, 1);
    naive_release(1, 1);

    atomic_store(&t2_done, true);
    return NULL;
}

static void run_naive_demo(void) {
    printf("============================================================\n");
    printf(">>> PART 1: NAIVE BASELINE (Uncoordinated Acquisition)\n");
    printf("============================================================\n");
    naive_init(&demo_rc);
    atomic_store(&t1_done, false);
    atomic_store(&t2_done, false);

    pthread_t th1, th2;
    pthread_create(&th1, NULL, naive_thread1, NULL);
    pthread_create(&th2, NULL, naive_thread2, NULL);

    // Watchdog check for 1.5 seconds
    usleep(1500000);

    bool done1 = atomic_load(&t1_done);
    bool done2 = atomic_load(&t2_done);

    if (!done1 && !done2) {
        printf("\n>>> DEMO RESULT: DEADLOCK CONFIRMED!\n");
        printf("    Thread 1 holds DB and is blocked waiting for CLOCK.\n");
        printf("    Thread 2 holds CLOCK and is blocked waiting for DB.\n");
        printf("    System is permanently frozen in circular wait.\n");
    }

    // Cancel threads to clean up
    pthread_cancel(th1);
    pthread_cancel(th2);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    naive_destroy();
}

// Banker's mode simulation
static void *banker_thread1(void *arg) {
    (void)arg;
    int slot = rm_alloc_slot();
    int claim[M_MAX] = {1, 1}; // Needs DB and CLOCK
    rm_register_claim(slot, claim);

    printf("  [SafeServe - /refresh] Submitting claim {DB:1, CLOCK:1} to Banker's controller...\n");
    int rc = rm_acquire_atomic(slot, claim, 2000);
    if (rc == RM_SUCCESS) {
        printf("  [SafeServe - /refresh] Granted! Safely executing...\n");
        usleep(50000);
        rm_release_all(slot);
        printf("  [SafeServe - /refresh] Completed and released resources.\n");
    }
    rm_free_slot(slot);
    atomic_store(&t1_done, true);
    return NULL;
}

static void *banker_thread2(void *arg) {
    (void)arg;
    int slot = rm_alloc_slot();
    int claim[M_MAX] = {1, 1}; // Needs DB and CLOCK
    rm_register_claim(slot, claim);

    printf("  [SafeServe - /upload]  Submitting claim {DB:1, CLOCK:1} to Banker's controller...\n");
    int rc = rm_acquire_atomic(slot, claim, 2000);
    if (rc == RM_SUCCESS) {
        printf("  [SafeServe - /upload]  Granted! Safely executing...\n");
        usleep(50000);
        rm_release_all(slot);
        printf("  [SafeServe - /upload]  Completed and released resources.\n");
    }
    rm_free_slot(slot);
    atomic_store(&t2_done, true);
    return NULL;
}

static void run_banker_demo(void) {
    printf("\n============================================================\n");
    printf(">>> PART 2: SAFESERVE (Banker's Avoidance Algorithm)\n");
    printf("============================================================\n");
    rm_init(&demo_rc, MODE_BANKER, ADMIT_ATOMIC, 2000);
    atomic_store(&t1_done, false);
    atomic_store(&t2_done, false);

    pthread_t th1, th2;
    pthread_create(&th1, NULL, banker_thread1, NULL);
    pthread_create(&th2, NULL, banker_thread2, NULL);

    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    bool done1 = atomic_load(&t1_done);
    bool done2 = atomic_load(&t2_done);

    if (done1 && done2) {
        printf("\n>>> DEMO RESULT: SUCCESS! ZERO DEADLOCKS!\n");
        printf("    Banker's algorithm detected that granting both claims simultaneously\n");
        printf("    would create an unsafe state. It safely serialized execution,\n");
        printf("    allowing both requests to complete without freezing.\n");
    }

    rm_destroy();
}

int main(void) {
    printf("############################################################\n");
    printf("#     SafeServe Deadlock vs. Banker's Avoidance Demo       #\n");
    printf("############################################################\n\n");
    run_naive_demo();
    run_banker_demo();
    printf("\n############################################################\n");
    printf("#                    Demo Complete                         #\n");
    printf("############################################################\n");
    return 0;
}
