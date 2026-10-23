#define _POSIX_C_SOURCE 200809L
#include "banker.h"
#include <string.h>

bool banker_is_safe(const int *available,
                    const int alloc[][M_MAX],
                    const int max_claim[][M_MAX],
                    const bool *active,
                    int n, int m) {
    if (!available || !alloc || !max_claim || !active || n <= 0 || m <= 0) {
        return true;
    }

    int work[M_MAX];
    for (int j = 0; j < m; j++) {
        work[j] = available[j];
    }

    bool finish[N_MAX];
    for (int i = 0; i < n; i++) {
        finish[i] = !active[i]; // Inactive slots are already considered finished
    }

    while (true) {
        bool found = false;
        for (int i = 0; i < n; i++) {
            if (!finish[i]) {
                bool can_satisfy = true;
                for (int j = 0; j < m; j++) {
                    int need = max_claim[i][j] - alloc[i][j];
                    if (need < 0) need = 0;
                    if (need > work[j]) {
                        can_satisfy = false;
                        break;
                    }
                }

                if (can_satisfy) {
                    for (int j = 0; j < m; j++) {
                        work[j] += alloc[i][j];
                    }
                    finish[i] = true;
                    found = true;
                    break; // Restart scan from beginning to maintain deterministic order
                }
            }
        }

        if (!found) {
            break;
        }
    }

    for (int i = 0; i < n; i++) {
        if (!finish[i]) {
            return false; // Unsafe state
        }
    }

    return true; // Safe state
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

    // 1. Verify request <= need and request <= available
    for (int j = 0; j < m; j++) {
        int need = max_claim[slot][j] - alloc[slot][j];
        if (request[j] > need) {
            return false; // Error: request exceeds declared maximum claim
        }
        if (request[j] > available[j]) {
            return false; // Resource not available currently
        }
    }

    // 2. Build tentative state
    int temp_avail[M_MAX];
    for (int j = 0; j < m; j++) {
        temp_avail[j] = available[j] - request[j];
    }

    // To avoid copying full N_MAX x M_MAX matrix, we check if banker_is_safe can work with
    // a virtual alloc. Or we can copy the alloc matrix or temporarily modify alloc[slot].
    // Since alloc is passed as const int[][M_MAX], let's create a local modified row:
    int temp_alloc_slot[M_MAX];
    for (int j = 0; j < m; j++) {
        temp_alloc_slot[j] = alloc[slot][j] + request[j];
    }

    // Run safety check on tentative state
    int work[M_MAX];
    for (int j = 0; j < m; j++) {
        work[j] = temp_avail[j];
    }

    bool finish[N_MAX];
    for (int i = 0; i < n; i++) {
        finish[i] = !active[i];
    }

    while (true) {
        bool found = false;
        for (int i = 0; i < n; i++) {
            if (!finish[i]) {
                bool can_satisfy = true;
                const int *cur_alloc = (i == slot) ? temp_alloc_slot : alloc[i];
                for (int j = 0; j < m; j++) {
                    int need = max_claim[i][j] - cur_alloc[j];
                    if (need < 0) need = 0;
                    if (need > work[j]) {
                        can_satisfy = false;
                        break;
                    }
                }

                if (can_satisfy) {
                    for (int j = 0; j < m; j++) {
                        work[j] += cur_alloc[j];
                    }
                    finish[i] = true;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            break;
        }
    }

    for (int i = 0; i < n; i++) {
        if (!finish[i]) {
            return false;
        }
    }

    return true;
}
