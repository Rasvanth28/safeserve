#ifndef SAFESERVE_RM_H
#define SAFESERVE_RM_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "config.h"

#define RM_SUCCESS    0
#define RM_TIMEOUT   -1
#define RM_ERROR     -2
#define RM_SHUTDOWN  -3

typedef struct {
    int m;
    int total[M_MAX];
    int available[M_MAX];
    char res_names[M_MAX][32];
    bool active[N_MAX];
    int max[N_MAX][M_MAX];
    int alloc[N_MAX][M_MAX];
    uint64_t ticket[N_MAX];
    bool aborted[N_MAX];
    server_mode_t mode;
    admit_mode_t admit_mode;
    int admit_timeout_ms;
    bool shutdown;
    uint64_t next_ticket;
    pthread_mutex_t lock;
    pthread_cond_t cv;
} rm_t;

int rm_init(const resource_config_t *rc, server_mode_t mode, admit_mode_t admit_mode, int admit_timeout_ms);
void rm_destroy(void);

// Reserves a slot for an in-flight request
int rm_alloc_slot(void);
void rm_free_slot(int slot);

// Registers declared claim for a slot
int rm_register_claim(int slot, const int claim[M_MAX]);

// Atomic reservation: Acquires entire claim or waits until safe.
// Returns RM_SUCCESS, RM_TIMEOUT, or RM_SHUTDOWN.
int rm_acquire_atomic(int slot, const int claim[M_MAX], int timeout_ms);

// Incremental/Individual resource acquisition
int rm_acquire(int slot, int res_idx, int count, int timeout_ms);

// Release an individual resource
int rm_release(int slot, int res_idx, int count);

// Releases all allocated resources held by slot and frees it
void rm_release_all(int slot);

// Notifies resource manager to abort all waiters on shutdown
void rm_shutdown(void);

// Returns active snapshot for /metrics
void rm_get_snapshot(int avail_out[M_MAX], int alloc_sum_out[M_MAX], int *active_slots);

#endif // SAFESERVE_RM_H
