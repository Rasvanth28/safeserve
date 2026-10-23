#define _POSIX_C_SOURCE 200809L
#include "naive.h"

bool naive_can_grant(const int *available, const int *request, int m) {
    for (int j = 0; j < m; j++) {
        if (request[j] > available[j]) return false;
    }
    return true;
}
