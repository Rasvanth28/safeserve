#define _POSIX_C_SOURCE 200809L
#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <string.h>

static FILE *log_fp = NULL;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

int log_init(const char *filepath) {
    pthread_mutex_lock(&log_mu);
    if (!filepath || strcmp(filepath, "stdout") == 0 || strlen(filepath) == 0) {
        log_fp = stdout;
    } else {
        log_fp = fopen(filepath, "a");
        if (!log_fp) {
            log_fp = stdout;
            pthread_mutex_unlock(&log_mu);
            return -1;
        }
    }
    pthread_mutex_unlock(&log_mu);
    return 0;
}

void log_close(void) {
    pthread_mutex_lock(&log_mu);
    if (log_fp && log_fp != stdout && log_fp != stderr) {
        fclose(log_fp);
    }
    log_fp = NULL;
    pthread_mutex_unlock(&log_mu);
}

void log_msg(log_level_t level, const char *fmt, ...) {
    pthread_mutex_lock(&log_mu);
    FILE *out = log_fp ? log_fp : stdout;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    const char *lvl_str = "INFO";
    switch (level) {
        case LOG_LEVEL_DEBUG: lvl_str = "DEBUG"; break;
        case LOG_LEVEL_INFO:  lvl_str = "INFO";  break;
        case LOG_LEVEL_WARN:  lvl_str = "WARN";  break;
        case LOG_LEVEL_ERROR: lvl_str = "ERROR"; break;
    }

    fprintf(out, "[%s.%03ld] [%s] ", time_str, ts.tv_nsec / 1000000L, lvl_str);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);
    pthread_mutex_unlock(&log_mu);
}

void log_request(const char *route, int status, double total_ms, double queue_ms, double admit_ms, size_t bytes) {
    pthread_mutex_lock(&log_mu);
    FILE *out = log_fp ? log_fp : stdout;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // Format: ts route status total_ms queue_ms admit_ms bytes
    fprintf(out, "[REQ] %s.%03ld %s %d %.2f %.2f %.2f %zu\n",
            time_str, ts.tv_nsec / 1000000L,
            route ? route : "-", status, total_ms, queue_ms, admit_ms, bytes);
    fflush(out);
    pthread_mutex_unlock(&log_mu);
}
