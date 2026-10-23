#define _POSIX_C_SOURCE 200809L
#include "banker.h"
#include <stddef.h>

// Core safety algorithm. `slot`/`request` describe an optional tentative grant
// (slot = -1 for none) so banker_can_grant never has to copy the tables.
//
// The scan runs over a compacted list of active slots, so a check costs
// O(k^2 * m) for k active requests instead of O(N_MAX^2 * m).
static int safety_scan(const int *available,
                       const int alloc[][M_MAX],
                       const int max_claim[][M_MAX],
                       const bool *active,
                       int n, int m,
                       int slot, const int *request,
                       int *seq_out, int *k_out) {
    int work[M_MAX];
    for (int j = 0; j < m; j++) {
        work[j] = available[j] - (request ? request[j] : 0);
    }

    int idx[N_MAX];
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (active[i]) idx[k++] = i;
    }
    if (k_out) *k_out = k;

    bool finish[N_MAX] = {false};
    int done = 0;

    while (done < k) {
        bool found = false;
        for (int p = 0; p < k; p++) {
            if (finish[p]) continue;
            int i = idx[p];

            bool can_satisfy = true;
            for (int j = 0; j < m; j++) {
                int held = alloc[i][j] + ((i == slot && request) ? request[j] : 0);
                int need = max_claim[i][j] - held;
                if (need > work[j]) {
                    can_satisfy = false;
                    break;
                }
            }

            if (can_satisfy) {
                for (int j = 0; j < m; j++) {
                    work[j] += alloc[i][j] + ((i == slot && request) ? request[j] : 0);
                }
                finish[p] = true;
                if (seq_out) seq_out[done] = i;
                done++;
                found = true;
            }
        }
        if (!found) break;
    }

    return done;
}

bool banker_is_safe(const int *available,
                    const int alloc[][M_MAX],
                    const int max_claim[][M_MAX],
                    const bool *active,
                    int n, int m) {
    if (!available || !alloc || !max_claim || !active || n <= 0 || m <= 0) {
        return true;
    }
    int k = 0;
    int done = safety_scan(available, alloc, max_claim, active, n, m, -1, NULL, NULL, &k);
    return done == k;
}

int banker_safe_sequence(const int *available,
                         const int alloc[][M_MAX],
                         const int max_claim[][M_MAX],
                         const bool *active,
                         int n, int m,
                         int *seq_out) {
    if (!available || !alloc || !max_claim || !active || n <= 0 || m <= 0) {
        return 0;
    }
    return safety_scan(available, alloc, max_claim, active, n, m, -1, NULL, seq_out, NULL);
}

bool banker_can_grant(const int *available,
                      const int alloc[][M_MAX],
                      const int max_claim[][M_MAX],
                      const bool *active,
                      int n, int m,
                      int slot,
                      const int *request) {
    if (!available || !alloc || !max_claim || !active || !request) return false;
    if (slot < 0 || slot >= n || !active[slot]) return false;

    // Step 1 and 2 of the resource-request algorithm.
    for (int j = 0; j < m; j++) {
        if (request[j] < 0) return false;
        if (request[j] > max_claim[slot][j] - alloc[slot][j]) {
            return false; // Exceeds declared maximum claim
        }
        if (request[j] > available[j]) {
            return false; // Not enough free instances right now
        }
    }

    // Step 3: pretend to grant, then run the safety algorithm.
    int k = 0;
    int done = safety_scan(available, alloc, max_claim, active, n, m, slot, request, NULL, &k);
    return done == k;
}
