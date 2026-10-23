#define _POSIX_C_SOURCE 200809L
#include "worker.h"
#include "net.h"
#include "http.h"
#include "rm.h"
#include "handlers.h"
#include "metrics.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static double timespec_diff_ms(const struct timespec *start, const struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static void *worker_thread_fn(void *arg) {
    server_context_t *ctx = (server_context_t *)arg;

    char recv_buf[HTTP_MAX_HEADER_SIZE + 4096];

    while (ctx->running) {
        job_t job;
        if (!queue_pop(&ctx->queue, &job)) {
            break; // Queue closed on shutdown
        }

        struct timespec t_pop;
        clock_gettime(CLOCK_MONOTONIC, &t_pop);
        double queue_ms = timespec_diff_ms(&job.t_accept, &t_pop);

        // Receive request
        ssize_t nread = recv(job.client_fd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (nread <= 0) {
            close(job.client_fd);
            continue;
        }
        recv_buf[nread] = '\0';

        http_request_t req;
        int parse_rc = http_parse_request(recv_buf, nread, &req);
        if (parse_rc != 0) {
            const char *err = "400 Bad Request\n";
            http_send_response(job.client_fd, 400, "text/plain", err, strlen(err), 0);
            metrics_inc_status(400);
            close(job.client_fd);
            continue;
        }

        // Allocate RM slot
        int slot = rm_alloc_slot();
        if (slot < 0) {
            const char *err = "503 Service Unavailable (Slot Pool Exhausted)\n";
            http_send_response(job.client_fd, 503, "text/plain", err, strlen(err), 1);
            metrics_inc_status(503);
            metrics_inc_admission_timeout();
            http_free_request(&req);
            close(job.client_fd);
            continue;
        }

        // Register route claim
        int claim[M_MAX];
        config_get_route_claim(&ctx->cfg.resources, req.path, claim);
        rm_register_claim(slot, claim);

        // Admission check / reservation
        struct timespec t_admit_start, t_admit_end;
        clock_gettime(CLOCK_MONOTONIC, &t_admit_start);

        int admit_rc = rm_acquire_atomic(slot, claim, ctx->cfg.admit_timeout_ms);

        clock_gettime(CLOCK_MONOTONIC, &t_admit_end);
        double admit_ms = timespec_diff_ms(&t_admit_start, &t_admit_end);

        if (admit_rc == RM_TIMEOUT) {
            const char *err = "503 Service Unavailable (Admission Timeout)\n";
            http_send_response(job.client_fd, 503, "text/plain", err, strlen(err), 1);
            metrics_inc_status(503);
            log_request(req.path, 503, queue_ms + admit_ms, queue_ms, admit_ms, strlen(err));
            rm_free_slot(slot);
            http_free_request(&req);
            close(job.client_fd);
            continue;
        } else if (admit_rc != RM_SUCCESS) {
            const char *err = "500 Internal Server Error (Admission Aborted)\n";
            http_send_response(job.client_fd, 500, "text/plain", err, strlen(err), 0);
            metrics_inc_status(500);
            rm_free_slot(slot);
            http_free_request(&req);
            close(job.client_fd);
            continue;
        }

        // Execute request handler
        metrics_inc_requests();
        handle_request(job.client_fd, &req, &ctx->cfg, slot);
        metrics_inc_status(200);

        // Release allocated resources
        rm_release_all(slot);
        rm_free_slot(slot);

        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        double total_ms = timespec_diff_ms(&job.t_accept, &t_end);

        metrics_record_latency(total_ms);
        log_request(req.path, 200, total_ms, queue_ms, admit_ms, 0);

        http_free_request(&req);
        close(job.client_fd);
    }

    return NULL;
}

int worker_pool_start(server_context_t *ctx) {
    if (!ctx) return -1;

    ctx->num_workers = ctx->cfg.workers > 0 ? ctx->cfg.workers : 16;
    ctx->worker_threads = malloc(sizeof(pthread_t) * ctx->num_workers);
    if (!ctx->worker_threads) return -1;

    ctx->running = true;
    for (int i = 0; i < ctx->num_workers; i++) {
        if (pthread_create(&ctx->worker_threads[i], NULL, worker_thread_fn, ctx) != 0) {
            LOG_E("Failed to spawn worker thread %d", i);
            return -1;
        }
    }

    LOG_I("Spawned %d worker threads", ctx->num_workers);
    return 0;
}

void worker_pool_stop(server_context_t *ctx) {
    if (!ctx || !ctx->worker_threads) return;

    ctx->running = false;
    queue_close(&ctx->queue);

    for (int i = 0; i < ctx->num_workers; i++) {
        pthread_join(ctx->worker_threads[i], NULL);
    }

    free(ctx->worker_threads);
    ctx->worker_threads = NULL;
    LOG_I("All worker threads joined successfully");
}
