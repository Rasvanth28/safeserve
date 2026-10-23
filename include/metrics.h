#ifndef SAFESERVE_METRICS_H
#define SAFESERVE_METRICS_H

#include <stddef.h>
#include <stdint.h>

void metrics_init(void);

void metrics_inc_requests(void);
void metrics_inc_status(int status_code);
void metrics_inc_queue_full(void);
void metrics_inc_admission_timeout(void);

void metrics_record_safety_check(uint64_t duration_ns);
void metrics_record_latency(double total_ms);

// Generates text/json formatted metrics report into buf
void metrics_generate_report(char *buf, size_t max_len);

#endif // SAFESERVE_METRICS_H
