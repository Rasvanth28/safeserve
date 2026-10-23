#ifndef SAFESERVE_DETECT_H
#define SAFESERVE_DETECT_H

#include "config.h"

// Detection baseline: Periodically scans the allocation and request matrices,
// identifies deadlocked cycles/sets, selects a victim, and signals abort.
int detect_init(const resource_config_t *rc, int interval_ms);
int detect_start(void);
void detect_stop(void);
void detect_destroy(void);

uint64_t detect_get_cycles_found(void);
uint64_t detect_get_victims_aborted(void);

#endif // SAFESERVE_DETECT_H
