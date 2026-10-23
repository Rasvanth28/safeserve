#ifndef SAFESERVE_BANKER_H
#define SAFESERVE_BANKER_H

#include <stdbool.h>
#include "config.h"

// Pure Banker's Safety Algorithm.
// available: array of length m (currently available resource instances)
// alloc: matrix [N_MAX][M_MAX] of currently allocated resources
// max_claim: matrix [N_MAX][M_MAX] of declared maximum claims
// active: boolean array [N_MAX] indicating which slots are active
// n: total slot capacity (<= N_MAX)
// m: number of resource types (<= M_MAX)
// Returns true if the system state is safe, false otherwise.
bool banker_is_safe(const int *available,
                    const int alloc[][M_MAX],
                    const int max_claim[][M_MAX],
                    const bool *active,
                    int n, int m);

// Checks if granting `request` to `slot` results in a safe state.
// 1. Checks request <= need (max_claim - alloc)
// 2. Checks request <= available
// 3. Simulates allocation and tests banker_is_safe.
// Returns true if safe to grant, false if must wait or invalid.
bool banker_can_grant(const int *available,
                      const int alloc[][M_MAX],
                      const int max_claim[][M_MAX],
                      const bool *active,
                      int n, int m,
                      int slot,
                      const int *request);

#endif // SAFESERVE_BANKER_H
