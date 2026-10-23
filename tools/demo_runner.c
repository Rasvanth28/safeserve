#define _POSIX_C_SOURCE 200809L
// In-process deadlock demo: the same two-thread workload under all four
// strategies, using the real resource manager.
//
//   /refresh thread: DB    -> (100 ms) -> CLOCK
//   /upload  thread: CLOCK -> (100 ms) -> DB
//
// One DB connection and one cache lock exist, so if both threads get their
// first resource, each waits forever for the other's (circular wait).
#include "rm.h"
#include "lockorder.h"
#include "config.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static resource_config_t demo_rc = {
    .m = 2,
    .res_names = {"DB", "CLOCK"},
    .total = {1, 1},
};

static struct timespec t_start;
static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *who, const char *fmt, const char *arg) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&print_mu);
    printf("  [%5.0f ms] %-9s ", timespec_diff_ms(&t_start, &now), who);
    printf(fmt, arg);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&print_mu);
}

typedef struct {
    const char *name;
    int first, second;
    server_mode_t mode;
    int rc;
    atomic_bool done;
} actor_t;

static void *actor_fn(void *arg) {
    actor_t *a = arg;
    int slot = rm_alloc_slot();
    int claim[M_MAX] = {1, 1};
    rm_register_claim(slot, claim);

    acq_step_t plan[2] = { {a->first, 1}, {a->second, 1} };
    if (a->mode == MODE_LOCKORDER) {
        lockorder_sort_plan(plan, 2);
        if (plan[0].res != a->first) say(a->name, "reorders its plan to %s first (global order)", demo_rc.res_names[plan[0].res]);
    }

    for (int s = 0; s < 2; s++) {
        const char *res = demo_rc.res_names[plan[s].res];
        say(a->name, "requests %s ...", res);
        a->rc = rm_acquire(slot, plan[s].res, 1, 3000);
        if (a->rc != RM_SUCCESS) {
            say(a->name, "-> %s", rm_strerror(a->rc));
            break;
        }
        say(a->name, "-> got %s", res);
        if (s == 0) sleep_ms(100);
    }

    if (a->rc == RM_SUCCESS) {
        sleep_ms(50);
        say(a->name, "finished its work and %s", "released DB + CLOCK");
    }
    rm_free_slot(slot);
    atomic_store(&a->done, true);
    return NULL;
}

static void run(server_mode_t mode, const char *title, const char *explain) {
    printf("\n------------------------------------------------------------\n");
    printf(" mode = %s  (%s)\n", config_mode_name(mode), title);
    printf("------------------------------------------------------------\n");

    rm_init(&demo_rc, mode, 3000, 100);
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    actor_t a = { .name = "/refresh", .first = 0, .second = 1, .mode = mode };
    actor_t b = { .name = "/upload", .first = 1, .second = 0, .mode = mode };
    atomic_init(&a.done, false);
    atomic_init(&b.done, false);

    pthread_t ta, tb;
    pthread_create(&ta, NULL, actor_fn, &a);
    sleep_ms(5);
    pthread_create(&tb, NULL, actor_fn, &b);

    for (int waited = 0; waited < 1500; waited += 10) {
        if (atomic_load(&a.done) && atomic_load(&b.done)) break;
        sleep_ms(10);
    }

    bool frozen = !atomic_load(&a.done) || !atomic_load(&b.done);
    if (frozen) {
        say("watchdog", "%s", "both threads still blocked after 1.5 s -> DEADLOCK");
        rm_shutdown(); // Unblock them so the demo can continue
    }
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    rm_snapshot_t s;
    rm_get_snapshot(&s);
    int ok = (a.rc == RM_SUCCESS) + (b.rc == RM_SUCCESS);
    printf("\n  RESULT: %s\n", frozen ? "DEADLOCK - neither request can ever finish"
                                      : (ok == 2 ? "both requests completed"
                                                 : "deadlock broken; one request aborted, one completed"));
    if (mode == MODE_BANKER) {
        printf("          Banker's refused %llu grant(s) that would have been unsafe.\n",
               (unsigned long long)s.unsafe_denials);
    }
    if (mode == MODE_DETECT) {
        printf("          Detector found %llu deadlock(s), aborted %llu victim(s).\n",
               (unsigned long long)s.deadlocks_found, (unsigned long long)s.victims_aborted);
    }
    printf("  WHY:    %s\n", explain);
    rm_destroy();
}

int main(void) {
    log_set_level(LOG_LEVEL_ERROR);
    printf("############################################################\n");
    printf("#   SafeServe: one workload, four deadlock strategies      #\n");
    printf("############################################################\n");
    printf("Resources: DB x1, CLOCK x1. Both requests claim {DB:1, CLOCK:1}.\n");
    printf("/refresh takes DB then CLOCK; /upload takes CLOCK then DB.\n");

    run(MODE_NAIVE, "no deadlock handling",
        "each holds one resource and waits for the other: circular wait.");
    run(MODE_BANKER, "avoidance",
        "granting CLOCK to /upload while /refresh holds DB leaves no safe\n"
        "          sequence, so /upload waits until /refresh finishes.");
    run(MODE_LOCKORDER, "prevention",
        "both take DB before CLOCK, so a cycle cannot form.");
    run(MODE_DETECT, "detection and recovery",
        "the deadlock happens, the detector finds it and aborts a victim\n"
        "          (the victim's request gets 503 and its work is lost).");
    printf("\n");
    return 0;
}
