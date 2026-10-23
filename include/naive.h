#ifndef SAFESERVE_NAIVE_H
#define SAFESERVE_NAIVE_H

#include "config.h"

// Naive baseline: Each resource is guarded by an individual mutex or semaphore.
// Handlers acquire resources one-by-one with no global ordering or safety checks.
int naive_init(const resource_config_t *rc);
void naive_destroy(void);

int naive_acquire(int res_idx, int count);
void naive_release(int res_idx, int count);

#endif // SAFESERVE_NAIVE_H
