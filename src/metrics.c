#define _POSIX_C_SOURCE 200809L
#include "metrics.h"
#include "rm.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

static atomic_uint_fast64_t m_requests_total;
static atomic_uint_fast64_t m_status_2xx;
static atomic_uint_fast64_t m_status_4xx;
static atomic_uint_fast64_t m_status_5xx;
static atomic_uint_fast64_t m_status_503;
static atomic_uint_fast64_t m_queue_full;
static atomic_uint_fast64_t m_admission_timeout;
static atomic_uint_fast64_t m_victim_responses;
static atomic_uint_fast64_t m_queue_timeout;
static atomic_uint_fast64_t m_safety_checks_total;
static atomic_uint_fast64_t m_safety_checks_total_ns;
static atomic_uint_fast64_t m_safety_check_max_ns;

// Latency percentiles come from a uniform reservoir sample (Algorithm R) so
// memory stays bounded however long the server runs.
#define LATENCY_SAMPLES 10000
static double latency_samples[LATENCY_SAMPLES];
static uint64_t latency_seen = 0;
static unsigned int latency_rng = 12345;
static pthread_mutex_t latency_mu = PTHREAD_MUTEX_INITIALIZER;

static int (*queue_probe)(void *) = NULL;
static void *queue_probe_arg = NULL;

void metrics_init(void) {
    atomic_store(&m_requests_total, 0);
    atomic_store(&m_status_2xx, 0);
    atomic_store(&m_status_4xx, 0);
    atomic_store(&m_status_5xx, 0);
    atomic_store(&m_status_503, 0);
    atomic_store(&m_queue_full, 0);
    atomic_store(&m_admission_timeout, 0);
    atomic_store(&m_victim_responses, 0);
    atomic_store(&m_queue_timeout, 0);
    atomic_store(&m_safety_checks_total, 0);
    atomic_store(&m_safety_checks_total_ns, 0);
    atomic_store(&m_safety_check_max_ns, 0);

    pthread_mutex_lock(&latency_mu);
    latency_seen = 0;
    pthread_mutex_unlock(&latency_mu);
}

void metrics_set_queue_probe(int (*probe)(void *), void *arg) {
    queue_probe = probe;
    queue_probe_arg = arg;
}

void metrics_inc_status(int status_code) {
    atomic_fetch_add(&m_requests_total, 1);
    if (status_code >= 200 && status_code < 300) {
        atomic_fetch_add(&m_status_2xx, 1);
    } else if (status_code >= 400 && status_code < 500) {
        atomic_fetch_add(&m_status_4xx, 1);
    } else if (status_code >= 500) {
        atomic_fetch_add(&m_status_5xx, 1);
        if (status_code == 503) atomic_fetch_add(&m_status_503, 1);
    }
}

void metrics_inc_queue_full(void) {
    atomic_fetch_add(&m_queue_full, 1);
}

void metrics_inc_admission_timeout(void) {
    atomic_fetch_add(&m_admission_timeout, 1);
}

void metrics_inc_victim_response(void) {
    atomic_fetch_add(&m_victim_responses, 1);
}

void metrics_inc_queue_timeout(void) {
    atomic_fetch_add(&m_queue_timeout, 1);
}

void metrics_record_safety_check(uint64_t duration_ns) {
    atomic_fetch_add(&m_safety_checks_total, 1);
    atomic_fetch_add(&m_safety_checks_total_ns, duration_ns);
    uint64_t prev = atomic_load(&m_safety_check_max_ns);
    while (duration_ns > prev &&
           !atomic_compare_exchange_weak(&m_safety_check_max_ns, &prev, duration_ns)) {
    }
}

void metrics_record_latency(double total_ms) {
    pthread_mutex_lock(&latency_mu);
    if (latency_seen < LATENCY_SAMPLES) {
        latency_samples[latency_seen] = total_ms;
    } else {
        uint64_t r = (uint64_t)rand_r(&latency_rng) % (latency_seen + 1);
        if (r < LATENCY_SAMPLES) latency_samples[r] = total_ms;
    }
    latency_seen++;
    pthread_mutex_unlock(&latency_mu);
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double percentile(const double *sorted, size_t n, double p) {
    if (n == 0) return 0.0;
    size_t idx = (size_t)(p * (double)(n - 1) + 0.5);
    return sorted[idx];
}

void metrics_generate_report(char *buf, size_t max_len, const char *mode, const char *admit_mode) {
    uint64_t sc_cnt = atomic_load(&m_safety_checks_total);
    uint64_t sc_ns = atomic_load(&m_safety_checks_total_ns);
    double avg_sc_us = (sc_cnt > 0) ? ((double)sc_ns / ((double)sc_cnt * 1000.0)) : 0.0;
    double max_sc_us = (double)atomic_load(&m_safety_check_max_ns) / 1000.0;

    double p50 = 0.0, p95 = 0.0, p99 = 0.0;
    static double sorted[LATENCY_SAMPLES];
    static pthread_mutex_t sort_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&sort_mu);
    pthread_mutex_lock(&latency_mu);
    size_t n = latency_seen < LATENCY_SAMPLES ? (size_t)latency_seen : LATENCY_SAMPLES;
    memcpy(sorted, latency_samples, sizeof(double) * n);
    pthread_mutex_unlock(&latency_mu);
    qsort(sorted, n, sizeof(double), compare_doubles);
    p50 = percentile(sorted, n, 0.50);
    p95 = percentile(sorted, n, 0.95);
    p99 = percentile(sorted, n, 0.99);
    pthread_mutex_unlock(&sort_mu);

    rm_snapshot_t snap;
    rm_get_snapshot(&snap);
    int qlen = queue_probe ? queue_probe(queue_probe_arg) : 0;

    size_t off = 0;
#define APPEND(...) do { \
        int w_ = snprintf(buf + off, off < max_len ? max_len - off : 0, __VA_ARGS__); \
        if (w_ > 0) off += (size_t)w_; \
    } while (0)

    APPEND("{\n");
    APPEND("  \"mode\": \"%s\",\n  \"admit_mode\": \"%s\",\n", mode, admit_mode);
    APPEND("  \"requests\": {\n"
           "    \"total\": %llu,\n"
           "    \"status_2xx\": %llu,\n"
           "    \"status_4xx\": %llu,\n"
           "    \"status_5xx\": %llu,\n"
           "    \"status_503\": %llu,\n"
           "    \"rejected_queue_full\": %llu,\n"
           "    \"shed_queue_timeout\": %llu,\n"
           "    \"timed_out_admission\": %llu,\n"
           "    \"aborted_victims\": %llu,\n"
           "    \"queue_length\": %d\n"
           "  },\n",
           (unsigned long long)atomic_load(&m_requests_total),
           (unsigned long long)atomic_load(&m_status_2xx),
           (unsigned long long)atomic_load(&m_status_4xx),
           (unsigned long long)atomic_load(&m_status_5xx),
           (unsigned long long)atomic_load(&m_status_503),
           (unsigned long long)atomic_load(&m_queue_full),
           (unsigned long long)atomic_load(&m_queue_timeout),
           (unsigned long long)atomic_load(&m_admission_timeout),
           (unsigned long long)atomic_load(&m_victim_responses),
           qlen);
    APPEND("  \"latency_ms\": {\n"
           "    \"samples\": %zu,\n"
           "    \"p50\": %.2f,\n"
           "    \"p95\": %.2f,\n"
           "    \"p99\": %.2f\n"
           "  },\n", n, p50, p95, p99);
    APPEND("  \"banker\": {\n"
           "    \"safety_checks_total\": %llu,\n"
           "    \"avg_safety_check_us\": %.2f,\n"
           "    \"max_safety_check_us\": %.2f,\n"
           "    \"unsafe_denials\": %llu\n"
           "  },\n",
           (unsigned long long)sc_cnt, avg_sc_us, max_sc_us,
           (unsigned long long)snap.unsafe_denials);
    APPEND("  \"detect\": {\n"
           "    \"deadlocks_found\": %llu,\n"
           "    \"victims_aborted\": %llu\n"
           "  },\n",
           (unsigned long long)snap.deadlocks_found,
           (unsigned long long)snap.victims_aborted);
    APPEND("  \"lockorder\": {\n    \"order_violations\": %llu\n  },\n",
           (unsigned long long)snap.order_violations);
    APPEND("  \"resources\": {\n"
           "    \"active_slots\": %d,\n"
           "    \"waiting_slots\": %d,\n"
           "    \"table\": [\n", snap.active_slots, snap.waiting_slots);
    for (int j = 0; j < snap.m; j++) {
        APPEND("      {\"name\": \"%s\", \"total\": %d, \"available\": %d, \"allocated\": %d, \"peak\": %d}%s\n",
               snap.res_names[j], snap.total[j], snap.available[j],
               snap.total[j] - snap.available[j], snap.peak[j], j + 1 < snap.m ? "," : "");
    }
    APPEND("    ]\n  },\n");

    // Live view: each in-flight request, what it holds and what it waits for
    rm_inflight_t fl[24];
    int nfl = rm_list_inflight(fl, 24);
    APPEND("  \"inflight\": [");
    for (int k = 0; k < nfl; k++) {
        APPEND("%s\n    {\"slot\": %d, \"route\": \"%s\", \"waiting\": %s, \"age_ms\": %.0f, \"holds\": {",
               k ? "," : "", fl[k].slot, fl[k].label, fl[k].waiting ? "true" : "false", fl[k].age_ms);
        bool first = true;
        for (int j = 0; j < snap.m; j++) {
            if (fl[k].alloc[j] <= 0) continue;
            APPEND("%s\"%s\": %d", first ? "" : ", ", snap.res_names[j], fl[k].alloc[j]);
            first = false;
        }
        APPEND("}, \"wants\": {");
        first = true;
        for (int j = 0; j < snap.m; j++) {
            if (fl[k].request[j] <= 0) continue;
            APPEND("%s\"%s\": %d", first ? "" : ", ", snap.res_names[j], fl[k].request[j]);
            first = false;
        }
        APPEND("}}");
    }
    APPEND("%s]\n}\n", nfl ? "\n  " : "");
#undef APPEND
}

// ---- Utilization sampler ----

static pthread_t sampler_tid;
static bool sampler_running = false;
static FILE *sampler_fp = NULL;
static int sampler_interval_ms = 100;
static pthread_mutex_t sampler_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sampler_cv = PTHREAD_COND_INITIALIZER;

static void *sampler_fn(void *arg) {
    (void)arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&sampler_mu);
    while (sampler_running) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        timespec_add_ms(&deadline, sampler_interval_ms);
        pthread_cond_timedwait(&sampler_cv, &sampler_mu, &deadline);
        if (!sampler_running) break;

        rm_snapshot_t snap;
        rm_get_snapshot(&snap);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        fprintf(sampler_fp, "%.0f", timespec_diff_ms(&t0, &now));
        for (int j = 0; j < snap.m; j++) {
            fprintf(sampler_fp, ",%d", snap.total[j] - snap.available[j]);
        }
        fprintf(sampler_fp, ",%d,%d\n", snap.active_slots, snap.waiting_slots);
        fflush(sampler_fp);
    }
    pthread_mutex_unlock(&sampler_mu);
    return NULL;
}

int metrics_start_sampler(const char *csv_path, int interval_ms) {
    if (!csv_path || !*csv_path) return -1;
    sampler_fp = fopen(csv_path, "w");
    if (!sampler_fp) return -1;

    rm_snapshot_t snap;
    rm_get_snapshot(&snap);
    fprintf(sampler_fp, "elapsed_ms");
    for (int j = 0; j < snap.m; j++) {
        fprintf(sampler_fp, ",%s_used", snap.res_names[j]);
    }
    fprintf(sampler_fp, ",active,waiting\n");

    sampler_interval_ms = interval_ms > 0 ? interval_ms : 100;
    sampler_running = true;
    if (pthread_create(&sampler_tid, NULL, sampler_fn, NULL) != 0) {
        sampler_running = false;
        fclose(sampler_fp);
        sampler_fp = NULL;
        return -1;
    }
    return 0;
}

void metrics_stop_sampler(void) {
    pthread_mutex_lock(&sampler_mu);
    if (!sampler_running) {
        pthread_mutex_unlock(&sampler_mu);
        return;
    }
    sampler_running = false;
    pthread_cond_signal(&sampler_cv);
    pthread_mutex_unlock(&sampler_mu);

    pthread_join(sampler_tid, NULL);
    fclose(sampler_fp);
    sampler_fp = NULL;
}
