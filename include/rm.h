#ifndef SAFESERVE_RM_H
#define SAFESERVE_RM_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "config.h"

#define RM_SUCCESS    0
#define RM_TIMEOUT   -1   // Waited admit_timeout_ms without a safe grant (-> 503)
#define RM_ERROR     -2   // Invalid call or request beyond the declared claim (-> 500)
#define RM_SHUTDOWN  -3   // Server is shutting down (-> 503)
#define RM_ABORTED   -4   // Chosen as a deadlock victim in detect mode (-> 503)
#define RM_ORDER     -5   // Lock-order violation in lockorder mode (-> 500)

// All resource tables live behind one mutex. need = max - alloc is derived,
// never stored. request[i] is non-zero only while slot i is blocked waiting.
typedef struct {
    int m;
    int total[M_MAX];
    int available[M_MAX];
    char res_names[M_MAX][32];

    bool active[N_MAX];
    int max[N_MAX][M_MAX];
    int alloc[N_MAX][M_MAX];
    int request[N_MAX][M_MAX];
    bool waiting[N_MAX];
    bool aborted[N_MAX];
    uint64_t ticket[N_MAX];        // Admission order (detect: victim tiebreak)
    uint64_t wait_ticket[N_MAX];   // Order of entering the wait loop (banker: FIFO fairness)
    struct timespec wait_since[N_MAX];

    server_mode_t mode;
    int admit_timeout_ms;
    int aging_ms;                  // banker: starvation guard threshold (0 = off)
    bool shutdown;
    uint64_t next_ticket;
    uint64_t next_wait_ticket;
    int n_active;
    int n_waiting;

    // Counters (read under lock via rm_get_snapshot)
    uint64_t safety_checks;
    uint64_t unsafe_denials;       // Units were free but granting would be unsafe
    uint64_t deadlocks_found;
    uint64_t victims_aborted;
    uint64_t order_violations;

    pthread_mutex_t lock;
    pthread_cond_t cv;
} rm_t;

typedef struct {
    int m;
    char res_names[M_MAX][32];
    int total[M_MAX];
    int available[M_MAX];
    int active_slots;
    int waiting_slots;
    uint64_t safety_checks;
    uint64_t unsafe_denials;
    uint64_t deadlocks_found;
    uint64_t victims_aborted;
    uint64_t order_violations;
} rm_snapshot_t;

// Detect mode starts the detector thread with detect_interval_ms.
int rm_init(const resource_config_t *rc, server_mode_t mode, int admit_timeout_ms, int detect_interval_ms);
void rm_destroy(void);

// Banker fairness (FR-17): once a waiter has waited aging_ms, requests from
// slots that hold nothing yet yield to it, so in-flight requests drain and the
// starved claim is served. 0 disables the guard. Default 250 ms.
void rm_set_fairness_aging(int aging_ms);

// Wakes every waiter with RM_SHUTDOWN; later acquisitions fail immediately.
void rm_shutdown(void);

// Reserves a slot (a Banker's "process") for one in-flight request.
// Returns the slot index, or -1 if all N_MAX slots are in use.
int rm_alloc_slot(void);

// Releases everything the slot holds and frees it.
void rm_free_slot(int slot);

// Declares the slot's maximum claim. Must be called before any acquisition.
// Returns RM_ERROR if the claim exceeds the system total (FR-15).
int rm_register_claim(int slot, const int claim[M_MAX]);

// Requests the vector `req` (all units at once). Blocks until the active
// strategy allows the grant:
//   banker    - grant only if the resulting state is safe; fair among waiters; times out
//   lockorder - grant if free and the global order is respected; times out
//   detect    - grant if free; waits until granted, aborted as victim, or shutdown
//   naive     - grant if free; waits until granted or shutdown (can deadlock)
// timeout_ms <= 0 uses the configured admission timeout.
int rm_acquire_vec(int slot, const int req[M_MAX], int timeout_ms);

// Convenience wrapper: `count` units of a single resource.
int rm_acquire(int slot, int res_idx, int count, int timeout_ms);

// Returns units to the pool and wakes waiters.
int rm_release(int slot, int res_idx, int count);
void rm_release_all(int slot);

// One detection pass (called by the detector thread; public for tests).
// Aborts victims until no deadlock remains. Returns the number of victims.
int rm_detect_and_recover(void);

// Remaining need (max - alloc) of a slot.
void rm_get_need(int slot, int need_out[M_MAX]);

void rm_get_snapshot(rm_snapshot_t *out);

// Human-readable name for an RM_* code.
const char *rm_strerror(int rc);

#endif // SAFESERVE_RM_H
