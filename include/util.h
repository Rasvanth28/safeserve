#ifndef SAFESERVE_UTIL_H
#define SAFESERVE_UTIL_H

#include <time.h>
#include <errno.h>

// usleep() is not part of POSIX.1-2008, so glibc hides it under
// _POSIX_C_SOURCE=200809L. Use nanosleep everywhere instead.
static inline void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static inline double timespec_diff_ms(const struct timespec *start, const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) * 1000.0 +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static inline void timespec_add_ms(struct timespec *ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

#endif // SAFESERVE_UTIL_H
