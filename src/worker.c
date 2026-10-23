#define _POSIX_C_SOURCE 200809L
#include "worker.h"
#include "net.h"
#include "http.h"
#include "rm.h"
#include "handlers.h"
#include "metrics.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

// Runs admission control and the handler for one parsed request.
// Returns the HTTP status sent and fills *admit_ms.
static int serve_request(server_context_t *ctx, int fd, const http_request_t *req, double *admit_ms) {
    *admit_ms = 0.0;

    const route_t *route = handlers_find_route(req->path);
    if (!route) {
        return send_error(fd, 404, NULL);
    }
    if (!(route->methods & METHOD_BIT(req->method))) {
        char allow[64];
        snprintf(allow, sizeof(allow), "Allow: %s\r\n", route->allow);
        const char *body = "405 Method Not Allowed\n";
        http_send_response_ex(fd, 405, "text/plain; charset=utf-8", body, strlen(body), allow);
        return 405;
    }

    int claim[M_MAX];
    config_get_route_claim(&ctx->cfg.resources, route->claim_path ? route->claim_path : req->path, claim);
    bool zero_claim = true;
    for (int j = 0; j < ctx->cfg.resources.m; j++) {
        if (claim[j] > 0) zero_claim = false;
    }

    // Zero-claim routes (/health, /metrics) bypass the resource manager so
    // they stay responsive even when every resource is held (EDD §16).
    if (zero_claim) {
        return route->fn(fd, req, &ctx->cfg, -1);
    }

    int slot = rm_alloc_slot();
    if (slot < 0) {
        return send_error(fd, 503, "request slots exhausted");
    }
    if (rm_register_claim(slot, claim) != RM_SUCCESS) {
        rm_free_slot(slot);
        return send_error(fd, 500, "route claim exceeds system capacity");
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int status;
    if (ctx->cfg.admit_mode == ADMIT_ATOMIC) {
        // Reserve the whole claim up front; the handler then runs with
        // everything it needs (no hold-and-wait).
        int rc = rm_acquire_vec(slot, claim, ctx->cfg.admit_timeout_ms);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        status = (rc == RM_SUCCESS) ? route->fn(fd, req, &ctx->cfg, slot) : send_rm_failure(fd, rc);
    } else {
        // Incremental: the handler acquires step by step, so admission wait
        // is part of the handler time.
        status = route->fn(fd, req, &ctx->cfg, slot);
        clock_gettime(CLOCK_MONOTONIC, &t1);
    }
    *admit_ms = timespec_diff_ms(&t0, &t1);

    rm_free_slot(slot);
    return status;
}

static void *worker_thread_fn(void *arg) {
    server_context_t *ctx = (server_context_t *)arg;

    job_t job;
    while (queue_pop(&ctx->queue, &job)) {
        struct timespec t_pop;
        clock_gettime(CLOCK_MONOTONIC, &t_pop);
        double queue_ms = timespec_diff_ms(&job.t_accept, &t_pop);

        http_request_t req;
        int read_rc = http_read_request(job.client_fd, &req);
        int status;
        double admit_ms = 0.0;

        if (read_rc < 0) {
            // Peer closed or reset before sending a request.
            http_free_request(&req);
            close(job.client_fd);
            continue;
        } else if (read_rc > 0) {
            status = send_error(job.client_fd, read_rc, NULL);
        } else if (ctx->cfg.queue_timeout_ms > 0 && queue_ms > ctx->cfg.queue_timeout_ms) {
            // Overloaded: the client has probably given up already. Shedding
            // stale work keeps latency bounded; serving abandoned requests
            // would let goodput collapse to zero.
            status = send_error(job.client_fd, 503, "queue wait exceeded");
            metrics_inc_queue_timeout();
        } else {
            status = serve_request(ctx, job.client_fd, &req, &admit_ms);
        }

        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double total_ms = timespec_diff_ms(&job.t_accept, &t_end);

        metrics_inc_status(status);
        metrics_record_latency(total_ms);
        log_request(read_rc == 0 ? req.path : "-", status, total_ms, queue_ms, admit_ms, req.body_len);

        http_free_request(&req);
        close(job.client_fd);
    }

    return NULL;
}

int worker_pool_start(server_context_t *ctx) {
    if (!ctx) return -1;

    ctx->num_workers = ctx->cfg.workers > 0 ? ctx->cfg.workers : 16;
    ctx->worker_threads = malloc(sizeof(pthread_t) * (size_t)ctx->num_workers);
    if (!ctx->worker_threads) return -1;

    for (int i = 0; i < ctx->num_workers; i++) {
        if (pthread_create(&ctx->worker_threads[i], NULL, worker_thread_fn, ctx) != 0) {
            LOG_E("Failed to spawn worker thread %d", i);
            ctx->num_workers = i;
            return -1;
        }
    }

    LOG_I("Spawned %d worker threads", ctx->num_workers);
    return 0;
}

void worker_pool_stop(server_context_t *ctx) {
    if (!ctx || !ctx->worker_threads) return;

    queue_close(&ctx->queue);

    for (int i = 0; i < ctx->num_workers; i++) {
        pthread_join(ctx->worker_threads[i], NULL);
    }

    free(ctx->worker_threads);
    ctx->worker_threads = NULL;
    LOG_I("All worker threads joined");
}
