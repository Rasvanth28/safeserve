#define _POSIX_C_SOURCE 200809L
#include "lockorder.h"

bool lockorder_order_ok(const int *held, const int *request, int m) {
    int highest_held = -1;
    int lowest_req = m;
    for (int j = 0; j < m; j++) {
        if (held[j] > 0) highest_held = j;
        if (request[j] > 0 && j < lowest_req) lowest_req = j;
    }
    return lowest_req > highest_held;
}

int lockorder_sort_plan(acq_step_t *steps, int n) {
    // Insertion sort: plans have at most a handful of steps.
    for (int i = 1; i < n; i++) {
        acq_step_t cur = steps[i];
        int k = i - 1;
        while (k >= 0 && steps[k].res > cur.res) {
            steps[k + 1] = steps[k];
            k--;
        }
        steps[k + 1] = cur;
    }

    int out = 0;
    for (int i = 0; i < n; i++) {
        if (out > 0 && steps[out - 1].res == steps[i].res) {
            steps[out - 1].count += steps[i].count;
        } else {
            steps[out++] = steps[i];
        }
    }
    return out;
}
