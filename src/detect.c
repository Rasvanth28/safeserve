#define _POSIX_C_SOURCE 200809L
#include "detect.h"
#include "rm.h"
#include "util.h"

#include <pthread.h>
#include <stdbool.h>
#include <time.h>

int detect_find_deadlocked(const int *available,
                           const int alloc[][M_MAX],
                           const int request[][M_MAX],
                           const bool *active,
                           int n, int m,
                           bool *deadlocked_out) {
    int work[M_MAX];
    for (int j = 0; j < m; j++) {
        work[j] = available[j];
    }

    // finish[i] starts true for slots that hold nothing.
    bool finish[N_MAX];
    for (int i = 0; i < n; i++) {
        finish[i] = true;
        if (!active[i]) continue;
        for (int j = 0; j < m; j++) {
            if (alloc[i][j] > 0) {
                finish[i] = false;
                break;
            }
        }
    }

    // Optimistically assume any slot whose current request fits will finish
    // and return what it holds.
    bool found = true;
    while (found) {
        found = false;
        for (int i = 0; i < n; i++) {
            if (finish[i]) continue;
            bool fits = true;
            for (int j = 0; j < m; j++) {
                if (request[i][j] > work[j]) {
                    fits = false;
                    break;
                }
            }
            if (fits) {
                for (int j = 0; j < m; j++) {
                    work[j] += alloc[i][j];
                }
                finish[i] = true;
                found = true;
            }
        }
    }

    int count = 0;
    for (int i = 0; i < n; i++) {
        deadlocked_out[i] = !finish[i];
        if (!finish[i]) count++;
    }
    return count;
}

int detect_pick_victim(const int alloc[][M_MAX],
                       const uint64_t *ticket,
                       const bool *deadlocked,
                       int n, int m) {
    int victim = -1;
    int best_held = 0;
    for (int i = 0; i < n; i++) {
        if (!deadlocked[i]) continue;
        int held = 0;
        for (int j = 0; j < m; j++) {
            held += alloc[i][j];
        }
        if (victim < 0 || held < best_held ||
            (held == best_held && ticket[i] > ticket[victim])) {
            victim = i;
            best_held = held;
        }
    }
    return victim;
}

// ---- Detector thread ----

static pthread_t detect_tid;
static bool detect_running = false;
static int detect_interval_ms = 200;
static pthread_mutex_t detect_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t detect_cv = PTHREAD_COND_INITIALIZER;

static void *detector_thread_fn(void *arg) {
    (void)arg;
    pthread_mutex_lock(&detect_mu);
    while (detect_running) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        timespec_add_ms(&deadline, detect_interval_ms);
        // Sleep on a condvar rather than nanosleep so detect_stop() is prompt.
        pthread_cond_timedwait(&detect_cv, &detect_mu, &deadline);
        if (!detect_running) break;

        pthread_mutex_unlock(&detect_mu);
        rm_detect_and_recover();
        pthread_mutex_lock(&detect_mu);
    }
    pthread_mutex_unlock(&detect_mu);
    return NULL;
}

int detect_start(int interval_ms) {
    pthread_mutex_lock(&detect_mu);
    if (detect_running) {
        pthread_mutex_unlock(&detect_mu);
        return 0;
    }
    detect_interval_ms = interval_ms > 0 ? interval_ms : 200;
    detect_running = true;
    if (pthread_create(&detect_tid, NULL, detector_thread_fn, NULL) != 0) {
        detect_running = false;
        pthread_mutex_unlock(&detect_mu);
        return -1;
    }
    pthread_mutex_unlock(&detect_mu);
    return 0;
}

void detect_stop(void) {
    pthread_mutex_lock(&detect_mu);
    if (!detect_running) {
        pthread_mutex_unlock(&detect_mu);
        return;
    }
    detect_running = false;
    pthread_cond_signal(&detect_cv);
    pthread_mutex_unlock(&detect_mu);

    pthread_join(detect_tid, NULL);
}
