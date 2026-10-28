#define _POSIX_C_SOURCE 200809L
#include "handlers.h"
#include "rm.h"
#include "lockorder.h"
#include "metrics.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define GET_HEAD (METHOD_BIT(HTTP_METHOD_GET) | METHOD_BIT(HTTP_METHOD_HEAD))
#define POST_ONLY METHOD_BIT(HTTP_METHOD_POST)

static const route_t routes[] = {
    { "/health",   "/health",            GET_HEAD,  "GET, HEAD", handle_health },
    { "/metrics",  "/metrics",           GET_HEAD,  "GET, HEAD", handle_metrics },
    { "/",         "/static/index.html", GET_HEAD,  "GET, HEAD", handle_static },
    { "/static",   "/static/index.html", GET_HEAD,  "GET, HEAD", handle_static },
    { "/static/*", NULL,                 GET_HEAD,  "GET, HEAD", handle_static },
    { "/query",    "/query",             GET_HEAD,  "GET, HEAD", handle_query },
    { "/upload",   "/upload",            POST_ONLY, "POST",      handle_upload },
    { "/refresh",  "/refresh",           POST_ONLY, "POST",      handle_refresh },
};

const route_t *handlers_find_route(const char *path) {
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const char *pat = routes[i].pattern;
        size_t len = strlen(pat);
        if (pat[len - 1] == '*') {
            if (strncmp(path, pat, len - 1) == 0) return &routes[i];
        } else if (strcmp(path, pat) == 0) {
            return &routes[i];
        }
    }
    return NULL;
}

int send_error(int sock_fd, int status_code, const char *detail) {
    char body[256];
    snprintf(body, sizeof(body), "%d %s%s%s\n", status_code, http_status_reason(status_code),
             detail ? ": " : "", detail ? detail : "");
    http_send_response(sock_fd, status_code, "text/plain; charset=utf-8",
                       body, strlen(body), status_code == 503 ? 1 : 0);
    return status_code;
}

int send_rm_failure(int sock_fd, int rm_rc) {
    int status = (rm_rc == RM_ERROR || rm_rc == RM_ORDER) ? 500 : 503;
    if (rm_rc == RM_ABORTED) metrics_inc_victim_response();
    return send_error(sock_fd, status, rm_strerror(rm_rc));
}

// Incremental acquisition (admit_mode = incremental). The handler takes its
// claimed resources one type at a time, in the order its logic needs them,
// holding what it already has while it waits for the next (hold-and-wait).
// `order` names the resource types; any claimed type not listed is taken last.
// hold_gap_ms is slept before the final step: the window in which two handlers
// with opposite orders can each grab their first lock.
//
// admit_timeout_ms bounds the total wait across all steps, not each step.
// In lockorder mode the plan is re-sorted into global index order first.
// Returns RM_SUCCESS or the failing RM_* code.
static int acquire_plan(int slot, const server_config_t *cfg,
                        const char *const *order, int n_order, int hold_gap_ms) {
    if (cfg->admit_mode != ADMIT_INCREMENTAL || slot < 0) return RM_SUCCESS;

    const resource_config_t *rc = &cfg->resources;
    int need[M_MAX];
    rm_get_need(slot, need);

    acq_step_t steps[M_MAX];
    int n = 0;
    bool used[M_MAX] = {false};

    for (int k = 0; k < n_order; k++) {
        int idx = config_get_resource_idx(rc, order[k]);
        if (idx >= 0 && !used[idx] && need[idx] > 0) {
            steps[n++] = (acq_step_t){ idx, need[idx] };
            used[idx] = true;
        }
    }
    for (int j = 0; j < rc->m; j++) {
        if (!used[j] && need[j] > 0) {
            steps[n++] = (acq_step_t){ j, need[j] };
        }
    }

    if (cfg->mode == MODE_LOCKORDER) {
        n = lockorder_sort_plan(steps, n);
    }

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int s = 0; s < n; s++) {
        if (s == n - 1 && n > 1) sleep_ms(hold_gap_ms);
        clock_gettime(CLOCK_MONOTONIC, &now);
        int remaining = cfg->admit_timeout_ms - (int)timespec_diff_ms(&t0, &now);
        if (remaining < 1) remaining = 1;
        int rc_acq = rm_acquire(slot, steps[s].res, steps[s].count, remaining);
        if (rc_acq != RM_SUCCESS) return rc_acq;
    }
    return RM_SUCCESS;
}

int handle_health(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    (void)cfg;
    (void)slot;
    const char *resp = "{\"status\":\"ok\"}\n";
    http_send_response(sock_fd, 200, "application/json", resp,
                       req->method == HTTP_METHOD_HEAD ? 0 : strlen(resp), 0);
    return 200;
}

int handle_metrics(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    (void)slot;
    char report[16384];
    metrics_generate_report(report, sizeof(report), config_mode_name(cfg->mode),
                            config_admit_mode_name(cfg->admit_mode));
    http_send_response(sock_fd, 200, "application/json", report,
                       req->method == HTTP_METHOD_HEAD ? 0 : strlen(report), 0);
    return 200;
}

int handle_static(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    static const char *const order[] = { "WORKER", "FD", "BUF" };
    int rc = acquire_plan(slot, cfg, order, 3, 0);
    if (rc != RM_SUCCESS) return send_rm_failure(sock_fd, rc);

    char filepath[MAX_PATH_LEN];
    if (http_sanitize_path(cfg->docroot, req->path, filepath, sizeof(filepath)) != 0) {
        return send_error(sock_fd, 404, NULL);
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        return send_error(sock_fd, 404, NULL);
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return send_error(sock_fd, 404, NULL);
    }

    http_send_file(sock_fd, http_guess_content_type(filepath), fd, (size_t)st.st_size,
                   req->method == HTTP_METHOD_HEAD);
    close(fd);
    return 200;
}

int handle_query(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    static const char *const order[] = { "WORKER", "FD", "BUF", "DB" };
    int rc = acquire_plan(slot, cfg, order, 4, 0);
    if (rc != RM_SUCCESS) return send_rm_failure(sock_fd, rc);

    sleep_ms(cfg->simulated_db_delay_ms);

    const char *resp = "{\"route\":\"/query\",\"status\":\"success\",\"data\":\"simulated db record\"}\n";
    http_send_response(sock_fd, 200, "application/json", resp,
                       req->method == HTTP_METHOD_HEAD ? 0 : strlen(resp), 0);
    return 200;
}

// Upload: buffer the body, take the cache write lock, then record the upload
// in the DB. Takes CLOCK before DB.
int handle_upload(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    static const char *const order[] = { "WORKER", "BUF", "FD", "CLOCK", "DB" };
    int rc = acquire_plan(slot, cfg, order, 5, cfg->step_delay_ms);
    if (rc != RM_SUCCESS) return send_rm_failure(sock_fd, rc);

    sleep_ms(cfg->simulated_upload_delay_ms);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"route\":\"/upload\",\"status\":\"success\",\"bytes_received\":%zu}\n",
             req->body_len);
    http_send_response(sock_fd, 200, "application/json", resp, strlen(resp), 0);
    return 200;
}

// Refresh: read from the DB, then rewrite the cache entry under the cache
// write lock. Takes DB before CLOCK - the opposite order to upload.
int handle_refresh(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    (void)req;
    static const char *const order[] = { "WORKER", "BUF", "FD", "DB", "CLOCK" };
    int rc = acquire_plan(slot, cfg, order, 5, cfg->step_delay_ms);
    if (rc != RM_SUCCESS) return send_rm_failure(sock_fd, rc);

    sleep_ms(cfg->simulated_refresh_delay_ms);

    const char *resp = "{\"route\":\"/refresh\",\"status\":\"success\",\"refreshed\":true}\n";
    http_send_response(sock_fd, 200, "application/json", resp, strlen(resp), 0);
    return 200;
}
