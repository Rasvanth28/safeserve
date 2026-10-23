#define _POSIX_C_SOURCE 200809L
#include "detect.h"
#include "rm.h"
#include "log.h"

#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>

static pthread_t detect_tid;
static bool detect_running = false;
static int detect_interval = 200; // ms
static uint64_t cycles_found = 0;
static uint64_t victims_aborted = 0;
static pthread_mutex_t detect_mu = PTHREAD_MUTEX_INITIALIZER;

extern rm_t g_rm; // Shared resource manager state

// Multi-instance deadlock detection algorithm:
// Similar to safety algorithm, but unallocated processes are marked finish = true,
// and we check if active waiting processes can have their requests satisfied by available.
// If any processes remain with finish == false, they are deadlocked!
static void run_detection_pass(void) {
    pthread_mutex_lock(&g_rm.lock);

    int m = g_rm.m;
    int work[M_MAX];
    for (int j = 0; j < m; j++) {
        work[j] = g_rm.available[j];
    }

    bool finish[N_MAX];
    for (int i = 0; i < N_MAX; i++) {
        if (!g_rm.active[i]) {
            finish[i] = true;
        } else {
            // If alloc is all zeros, it does not hold resources causing deadlock
            bool holds_any = false;
            for (int j = 0; j < m; j++) {
                if (g_rm.alloc[i][j] > 0) {
                    holds_any = true;
                    break;
                }
            }
            finish[i] = !holds_any;
        }
    }

    // Step 2 & 3: Find i such that finish[i] == false and request[i] <= work
    while (true) {
        bool found = false;
        for (int i = 0; i < N_MAX; i++) {
            if (!finish[i]) {
                bool can_satisfy = true;
                for (int j = 0; j < m; j++) {
                    int req = g_rm.max[i][j] - g_rm.alloc[i][j];
                    if (req < 0) req = 0;
                    if (req > work[j]) {
                        can_satisfy = false;
                        break;
                    }
                }
                if (can_satisfy) {
                    for (int j = 0; j < m; j++) {
                        work[j] += g_rm.alloc[i][j];
                    }
                    finish[i] = true;
                    found = true;
                    break;
                }
            }
        }
        if (!found) break;
    }

    // Deadlock check: Any finish[i] == false is deadlocked
    int deadlocked_count = 0;
    int victim_slot = -1;
    int min_alloc = 999999;

    for (int i = 0; i < N_MAX; i++) {
        if (!finish[i]) {
            deadlocked_count++;
            // Calculate total allocation for victim selection
            int total_alloc = 0;
            for (int j = 0; j < m; j++) {
                total_alloc += g_rm.alloc[i][j];
            }
            if (total_alloc < min_alloc) {
                min_alloc = total_alloc;
                victim_slot = i;
            }
        }
    }

    if (deadlocked_count > 0 && victim_slot >= 0) {
        cycles_found++;
        victims_aborted++;
        LOG_W("Deadlock detected (%d processes)! Aborting victim slot %d", deadlocked_count, victim_slot);

        // Abort victim
        g_rm.aborted[victim_slot] = true;

        // Release victim's resources
        for (int j = 0; j < m; j++) {
            g_rm.available[j] += g_rm.alloc[victim_slot][j];
            g_rm.alloc[victim_slot][j] = 0;
        }

        // Wake all waiting threads
        pthread_cond_broadcast(&g_rm.cv);
    }

    pthread_mutex_unlock(&g_rm.lock);
}

static void *detector_thread_fn(void *arg) {
    (void)arg;
    while (true) {
        pthread_mutex_lock(&detect_mu);
        if (!detect_running) {
            pthread_mutex_unlock(&detect_mu);
            break;
        }
        pthread_mutex_unlock(&detect_mu);

        usleep(detect_interval * 1000);

        pthread_mutex_lock(&detect_mu);
        if (!detect_running) {
            pthread_mutex_unlock(&detect_mu);
            break;
        }
        pthread_mutex_unlock(&detect_mu);

        run_detection_pass();
    }
    return NULL;
}

int detect_init(const resource_config_t *rc, int interval_ms) {
    (void)rc;
    detect_interval = interval_ms > 0 ? interval_ms : 200;
    cycles_found = 0;
    victims_aborted = 0;
    detect_running = false;
    return 0;
}

int detect_start(void) {
    pthread_mutex_lock(&detect_mu);
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
    pthread_mutex_unlock(&detect_mu);

    pthread_join(detect_tid, NULL);
}

void detect_destroy(void) {
    detect_stop();
}

uint64_t detect_get_cycles_found(void) {
    return cycles_found;
}

uint64_t detect_get_victims_aborted(void) {
    return victims_aborted;
}
