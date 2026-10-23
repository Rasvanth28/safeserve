#ifndef SAFESERVE_DETECT_H
#define SAFESERVE_DETECT_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// Detection-and-recovery baseline. Requests are granted whenever units are
// free (like naive), but a background thread periodically runs the
// multi-instance detection algorithm (Silberschatz §8.7.2) and aborts a victim
// from each deadlocked set.

// Pure detection algorithm over the Allocation and Request matrices.
// request[i] is what slot i is blocked waiting for right now (all zeros when it
// is running). Slots that hold nothing cannot be part of a deadlock.
// Sets deadlocked_out[i] for every deadlocked slot and returns how many there are.
int detect_find_deadlocked(const int *available,
                           const int alloc[][M_MAX],
                           const int request[][M_MAX],
                           const bool *active,
                           int n, int m,
                           bool *deadlocked_out);

// Picks the victim among deadlocked slots: fewest held instances (least work
// lost), ties broken by the youngest (largest) ticket. Returns -1 if none.
int detect_pick_victim(const int alloc[][M_MAX],
                       const uint64_t *ticket,
                       const bool *deadlocked,
                       int n, int m);

// Starts/stops the detector thread, which calls rm_detect_and_recover() every
// interval_ms milliseconds.
int detect_start(int interval_ms);
void detect_stop(void);

#endif // SAFESERVE_DETECT_H
