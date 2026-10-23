#ifndef SAFESERVE_METRICS_H
#define SAFESERVE_METRICS_H

#include <stddef.h>
#include <stdint.h>

void metrics_init(void);

// Called once per response, with its real status code.
void metrics_inc_status(int status_code);
void metrics_inc_queue_full(void);
void metrics_inc_admission_timeout(void);
void metrics_inc_victim_response(void);
void metrics_inc_queue_timeout(void);

void metrics_record_safety_check(uint64_t duration_ns);
void metrics_record_latency(double total_ms);

// Optional probe so /metrics can report the request-queue length.
void metrics_set_queue_probe(int (*probe)(void *), void *arg);

// Writes the JSON report served at /metrics.
void metrics_generate_report(char *buf, size_t max_len, const char *mode, const char *admit_mode);

// Resource-utilization sampler (EDD §10): appends one CSV row per interval
// with elapsed ms and units in use per resource type. Returns 0 on success.
int metrics_start_sampler(const char *csv_path, int interval_ms);
void metrics_stop_sampler(void);

#endif // SAFESERVE_METRICS_H
