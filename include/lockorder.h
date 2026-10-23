#ifndef SAFESERVE_LOCKORDER_H
#define SAFESERVE_LOCKORDER_H

#include "config.h"

// Prevention baseline: Enforces strict monotonic ordering on resource indices.
int lockorder_init(const resource_config_t *rc);
void lockorder_destroy(void);

int lockorder_acquire_atomic(const int claim[M_MAX]);
void lockorder_release_all(const int claim[M_MAX]);

#endif // SAFESERVE_LOCKORDER_H
