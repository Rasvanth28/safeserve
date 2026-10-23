#ifndef SAFESERVE_LOG_H
#define SAFESERVE_LOG_H

#include <stddef.h>

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

int log_init(const char *filepath);
void log_close(void);

void log_msg(log_level_t level, const char *fmt, ...);
void log_request(const char *route, int status, double total_ms, double queue_ms, double admit_ms, size_t bytes);

#define LOG_D(fmt, ...) log_msg(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) log_msg(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) log_msg(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) log_msg(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#endif // SAFESERVE_LOG_H
