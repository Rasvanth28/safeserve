#ifndef SAFESERVE_LOCKORDER_H
#define SAFESERVE_LOCKORDER_H

#include <stdbool.h>
#include "config.h"

// Prevention baseline: every resource type has a fixed global index (its
// position in resources.conf). A request may only ask for types whose index is
// strictly greater than every type it already holds. A cycle in the wait-for
// graph would need some request to go "downhill", so circular wait is impossible.

// Returns true if a slot holding `held` may request `request` without
// violating the global order.
bool lockorder_order_ok(const int *held, const int *request, int m);

// Sorts an acquisition plan into ascending resource-index order and merges
// steps for the same resource, so a handler written in any order complies.
// Returns the new number of steps.
int lockorder_sort_plan(acq_step_t *steps, int n);

#endif // SAFESERVE_LOCKORDER_H
