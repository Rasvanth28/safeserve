#define _POSIX_C_SOURCE 200809L
#include "metrics.h"
#include "rm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

static atomic_uint_fast64_t m_requests_total;
static atomic_uint_fast64_t m_status_2xx;
static atomic_uint_fast64_t m_status_4xx;
static atomic_uint_fast64_t m_status_5xx;
static atomic_uint_fast64_t m_queue_full;
static atomic_uint_fast64_t m_admission_timeout;
static atomic_uint_fast64_t m_safety_checks_total;
static atomic_uint_fast64_t m_safety_checks_total_ns;

#define LATENCY_SAMPLES 10000
static double latency_samples[LATENCY_SAMPLES];
static size_t latency_count = 0;
static pthread_mutex_t latency_mu = PTHREAD_MUTEX_INITIALIZER;

void metrics_init(void) {
    atomic_init(&m_requests_total, 0);
    atomic_init(&m_status_2xx, 0);
    atomic_init(&m_status_4xx, 0);
    atomic_init(&m_status_5xx, 0);
    atomic_init(&m_queue_full, 0);
    atomic_init(&m_admission_timeout, 0);
    atomic_init(&m_safety_checks_total, 0);
    atomic_init(&m_safety_checks_total_ns, 0);

    pthread_mutex_lock(&latency_mu);
    latency_count = 0;
    pthread_mutex_unlock(&latency_mu);
}

void metrics_inc_requests(void) {
    atomic_fetch_add(&m_requests_total, 1);
}

void metrics_inc_status(int status_code) {
    if (status_code >= 200 && status_code < 300) {
        atomic_fetch_add(&m_status_2xx, 1);
    } else if (status_code >= 400 && status_code < 500) {
        atomic_fetch_add(&m_status_4xx, 1);
    } else if (status_code >= 500) {
        atomic_fetch_add(&m_status_5xx, 1);
    }
}

void metrics_inc_queue_full(void) {
    atomic_fetch_add(&m_queue_full, 1);
}

void metrics_inc_admission_timeout(void) {
    atomic_fetch_add(&m_admission_timeout, 1);
}

void metrics_record_safety_check(uint64_t duration_ns) {
    atomic_fetch_add(&m_safety_checks_total, 1);
    atomic_fetch_add(&m_safety_checks_total_ns, duration_ns);
}

void metrics_record_latency(double total_ms) {
    pthread_mutex_lock(&latency_mu);
    if (latency_count < LATENCY_SAMPLES) {
        latency_samples[latency_count++] = total_ms;
    } else {
        // reservoir sampling or ring buffer
        size_t idx = rand() % LATENCY_SAMPLES;
        latency_samples[idx] = total_ms;
    }
    pthread_mutex_unlock(&latency_mu);
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

void metrics_generate_report(char *buf, size_t max_len) {
    uint64_t total_req = atomic_load(&m_requests_total);
    uint64_t s2xx = atomic_load(&m_status_2xx);
    uint64_t s4xx = atomic_load(&m_status_4xx);
    uint64_t s5xx = atomic_load(&m_status_5xx);
    uint64_t qfull = atomic_load(&m_queue_full);
    uint64_t adm_to = atomic_load(&m_admission_timeout);
    uint64_t sc_cnt = atomic_load(&m_safety_checks_total);
    uint64_t sc_ns = atomic_load(&m_safety_checks_total_ns);
    double avg_sc_us = (sc_cnt > 0) ? ((double)sc_ns / (sc_cnt * 1000.0)) : 0.0;

    // Compute percentiles
    double p50 = 0.0, p95 = 0.0, p99 = 0.0;
    pthread_mutex_lock(&latency_mu);
    if (latency_count > 0) {
        double *copy = malloc(sizeof(double) * latency_count);
        if (copy) {
            memcpy(copy, latency_samples, sizeof(double) * latency_count);
            qsort(copy, latency_count, sizeof(double), compare_doubles);
            p50 = copy[(size_t)(latency_count * 0.50)];
            p95 = copy[(size_t)(latency_count * 0.95)];
            p99 = copy[(size_t)(latency_count * 0.99)];
            free(copy);
        }
    }
    pthread_mutex_unlock(&latency_mu);

    int avail[M_MAX], alloc_sum[M_MAX], active_slots = 0;
    rm_get_snapshot(avail, alloc_sum, &active_slots);

    snprintf(buf, max_len,
             "{\n"
             "  \"requests\": {\n"
             "    \"total\": %llu,\n"
             "    \"status_2xx\": %llu,\n"
             "    \"status_4xx\": %llu,\n"
             "    \"status_5xx\": %llu,\n"
             "    \"rejected_queue_full\": %llu,\n"
             "    \"timed_out_admission\": %llu\n"
             "  },\n"
             "  \"latency_ms\": {\n"
             "    \"p50\": %.2f,\n"
             "    \"p95\": %.2f,\n"
             "    \"p99\": %.2f\n"
             "  },\n"
             "  \"banker\": {\n"
             "    \"safety_checks_total\": %llu,\n"
             "    \"avg_safety_check_us\": %.2f\n"
             "  },\n"
             "  \"resources\": {\n"
             "    \"active_slots\": %d\n"
             "  }\n"
             "}\n",
             (unsigned long long)total_req,
             (unsigned long long)s2xx,
             (unsigned long long)s4xx,
             (unsigned long long)s5xx,
             (unsigned long long)qfull,
             (unsigned long long)adm_to,
             p50, p95, p99,
             (unsigned long long)sc_cnt,
             avg_sc_us,
             active_slots);
}
