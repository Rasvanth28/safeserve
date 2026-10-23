#ifndef SAFESERVE_BANKER_H
#define SAFESERVE_BANKER_H

#include <stdbool.h>
#include "config.h"

// Pure Banker's algorithm (Dijkstra; Silberschatz §8.6.3). No locking: callers
// pass a consistent snapshot of the tables.
//
// available: length m, currently free instances per resource type
// alloc:     [n][M_MAX] instances currently held by each slot
// max_claim: [n][M_MAX] declared maximum claim of each slot
// active:    [n] which slots take part in the check
// n:         number of slots to consider (<= N_MAX)
// m:         number of resource types (<= M_MAX)

// Returns true if the state is safe, i.e. some ordering lets every active slot
// obtain its remaining need and finish.
bool banker_is_safe(const int *available,
                    const int alloc[][M_MAX],
                    const int max_claim[][M_MAX],
                    const bool *active,
                    int n, int m);

// Like banker_is_safe, but also writes the safe sequence it found into seq_out
// (slot indices, in completion order). Returns the length of the sequence; the
// state is safe iff that equals the number of active slots.
int banker_safe_sequence(const int *available,
                         const int alloc[][M_MAX],
                         const int max_claim[][M_MAX],
                         const bool *active,
                         int n, int m,
                         int *seq_out);

// Resource-request algorithm: returns true if `request` for `slot` can be
// granted now, i.e. request <= need, request <= available, and the state after
// a tentative grant is safe. Does not modify any table.
bool banker_can_grant(const int *available,
                      const int alloc[][M_MAX],
                      const int max_claim[][M_MAX],
                      const bool *active,
                      int n, int m,
                      int slot,
                      const int *request);

#endif // SAFESERVE_BANKER_H
