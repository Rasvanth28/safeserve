#ifndef SAFESERVE_NAIVE_H
#define SAFESERVE_NAIVE_H

#include <stdbool.h>
#include "config.h"

// Naive baseline: a request is granted whenever the units are free, with no
// ordering rule, no safety check and no timeout. Handlers hold what they have
// while waiting for more (hold-and-wait), so two handlers that take DB and
// CLOCK in opposite orders deadlock and stay blocked forever.
bool naive_can_grant(const int *available, const int *request, int m);

#endif // SAFESERVE_NAIVE_H
