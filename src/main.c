#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "net.h"
#include "http.h"
#include "queue.h"
#include "worker.h"
#include "handlers.h"
#include "rm.h"
#include "metrics.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define SHUTDOWN_GRACE_MS 3000

static volatile sig_atomic_t g_keep_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static int queue_len_probe(void *arg) {
    return queue_count((queue_t *)arg);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --config <path>          Path to server.conf (default: config/server.conf)\n");
    printf("  --resources <path>       Path to resources.conf (default: config/resources.conf)\n");
    printf("  --mode <mode>            Deadlock strategy: banker | naive | lockorder | detect\n");
    printf("  --admit-mode <mode>      atomic (whole claim up front) | incremental (step by step)\n");
    printf("  --port <port>            TCP port to listen on\n");
    printf("  --workers <count>        Number of worker threads\n");
    printf("  --docroot <dir>          Directory served under /static\n");
    printf("  --detect-interval <ms>   Detector period in detect mode\n");
    printf("  --util-csv <path>        Write resource-utilization samples to a CSV file\n");
    printf("  --quiet                  Do not log every request\n");
    printf("  --help                   Display this help message\n");
}

int main(int argc, char *argv[]) {
    const char *server_conf_path = "config/server.conf";
    const char *resources_conf_path = "config/resources.conf";
    const char *cli_mode = NULL;
    const char *cli_admit = NULL;
    const char *cli_docroot = NULL;
    const char *cli_util_csv = NULL;
    int cli_port = -1;
    int cli_workers = -1;
    int cli_detect_interval = -1;
    bool quiet = false;

    for (int i = 1; i < argc; i++) {
        bool has_val = i + 1 < argc;
        if (strcmp(argv[i], "--config") == 0 && has_val) {
            server_conf_path = argv[++i];
        } else if (strcmp(argv[i], "--resources") == 0 && has_val) {
            resources_conf_path = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && has_val) {
            cli_mode = argv[++i];
        } else if (strcmp(argv[i], "--admit-mode") == 0 && has_val) {
            cli_admit = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && has_val) {
            cli_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--workers") == 0 && has_val) {
            cli_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--docroot") == 0 && has_val) {
            cli_docroot = argv[++i];
        } else if (strcmp(argv[i], "--detect-interval") == 0 && has_val) {
            cli_detect_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--util-csv") == 0 && has_val) {
            cli_util_csv = argv[++i];
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    server_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    log_init("stdout");

    if (config_load(server_conf_path, resources_conf_path, &ctx.cfg) != 0) {
        LOG_E("Failed to load configuration files");
        return 1;
    }

    // CLI overrides
    if (cli_mode && config_parse_mode(cli_mode, &ctx.cfg.mode) != 0) {
        LOG_E("Unknown mode '%s'", cli_mode);
        return 2;
    }
    if (cli_admit && config_parse_admit_mode(cli_admit, &ctx.cfg.admit_mode) != 0) {
        LOG_E("Unknown admit mode '%s'", cli_admit);
        return 2;
    }
    if (cli_port > 0) ctx.cfg.port = cli_port;
    if (cli_workers > 0) ctx.cfg.workers = cli_workers;
    if (cli_detect_interval > 0) ctx.cfg.detect_interval_ms = cli_detect_interval;
    if (cli_docroot) {
        strncpy(ctx.cfg.docroot, cli_docroot, sizeof(ctx.cfg.docroot) - 1);
    }
    if (cli_util_csv) {
        strncpy(ctx.cfg.util_csv, cli_util_csv, sizeof(ctx.cfg.util_csv) - 1);
    }

    if (config_validate(&ctx.cfg) != 0) {
        return 1;
    }

    if (strcmp(ctx.cfg.log_file, "stdout") != 0 && strlen(ctx.cfg.log_file) > 0) {
        log_init(ctx.cfg.log_file);
    }
    log_set_requests(!quiet);

    LOG_I("Starting SafeServe v1.1 on port %d", ctx.cfg.port);
    LOG_I("Deadlock strategy: %s | Admission mode: %s",
          config_mode_name(ctx.cfg.mode), config_admit_mode_name(ctx.cfg.admit_mode));
    LOG_I("Workers: %d | Queue capacity: %d | Docroot: %s",
          ctx.cfg.workers, ctx.cfg.queue_capacity, ctx.cfg.docroot);

    // Block SIGINT/SIGTERM while spawning threads so only the acceptor
    // (main) thread receives them and its accept() is interrupted.
    sigset_t sigs, old_sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, &old_sigs);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;   // No SA_RESTART: accept() returns EINTR
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    metrics_init();

    if (rm_init(&ctx.cfg.resources, ctx.cfg.mode, ctx.cfg.admit_timeout_ms, ctx.cfg.detect_interval_ms) != 0) {
        LOG_E("Failed to initialize Resource Manager");
        return 1;
    }
    rm_set_fairness_aging(ctx.cfg.fairness_aging_ms);

    if (queue_init(&ctx.queue, ctx.cfg.queue_capacity) != 0) {
        LOG_E("Failed to initialize job queue");
        rm_destroy();
        return 1;
    }
    metrics_set_queue_probe(queue_len_probe, &ctx.queue);

    ctx.listen_fd = net_listen(ctx.cfg.port, 128);
    if (ctx.listen_fd < 0) {
        LOG_E("Failed to bind and listen on port %d", ctx.cfg.port);
        queue_destroy(&ctx.queue);
        rm_destroy();
        return 1;
    }

    if (worker_pool_start(&ctx) != 0) {
        LOG_E("Failed to start worker pool");
        close(ctx.listen_fd);
        worker_pool_stop(&ctx);
        queue_destroy(&ctx.queue);
        rm_destroy();
        return 1;
    }

    if (ctx.cfg.util_csv[0] != '\0') {
        if (metrics_start_sampler(ctx.cfg.util_csv, ctx.cfg.util_interval_ms) != 0) {
            LOG_W("Could not open utilization CSV %s", ctx.cfg.util_csv);
        }
    }

    pthread_sigmask(SIG_SETMASK, &old_sigs, NULL);

    LOG_I("SafeServe listening on 0.0.0.0:%d", ctx.cfg.port);

    // Acceptor loop
    while (g_keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(ctx.listen_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno != EINTR && g_keep_running) {
                LOG_W("accept() failed: %s", strerror(errno));
                sleep_ms(10); // e.g. EMFILE: back off instead of spinning
            }
            continue;
        }

        // Receive timeout so slow or idle clients cannot pin a worker (slowloris)
        net_set_recv_timeout(client_fd, 5);

        job_t job;
        job.client_fd = client_fd;
        clock_gettime(CLOCK_MONOTONIC, &job.t_accept);

        if (!queue_try_push(&ctx.queue, job)) {
            // Queue full: reject immediately with 503 (FR-8)
            send_error(client_fd, 503, "queue full");
            metrics_inc_queue_full();
            metrics_inc_status(503);
            close(client_fd);
        }
    }

    // Graceful shutdown (EDD §7): stop accepting, let queued and in-flight
    // requests finish for a grace period, then wake anything still blocked
    // in the resource manager (e.g. a naive-mode deadlock) and join.
    LOG_I("Shutting down SafeServe...");
    close(ctx.listen_fd);
    queue_close(&ctx.queue);

    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (true) {
        rm_snapshot_t snap;
        rm_get_snapshot(&snap);
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((snap.active_slots == 0 && queue_count(&ctx.queue) == 0) ||
            timespec_diff_ms(&t0, &now) > SHUTDOWN_GRACE_MS) {
            if (snap.active_slots > 0) {
                LOG_W("%d request(s) still blocked after %d ms; aborting them",
                      snap.active_slots, SHUTDOWN_GRACE_MS);
            }
            break;
        }
        sleep_ms(50);
    }

    rm_shutdown();
    worker_pool_stop(&ctx);
    metrics_stop_sampler();
    queue_destroy(&ctx.queue);
    rm_destroy();
    LOG_I("SafeServe stopped");
    log_close();
    return 0;
}
