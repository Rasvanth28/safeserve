#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "net.h"
#include "http.h"
#include "queue.h"
#include "worker.h"
#include "rm.h"
#include "metrics.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static volatile sig_atomic_t g_keep_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --config <path>      Path to server.conf (default: config/server.conf)\n");
    printf("  --resources <path>   Path to resources.conf (default: config/resources.conf)\n");
    printf("  --mode <mode>        Deadlock mode: banker | naive | lockorder | detect\n");
    printf("  --port <port>        TCP port to listen on\n");
    printf("  --workers <count>    Number of worker threads\n");
    printf("  --help               Display this help message\n");
}

int main(int argc, char *argv[]) {
    const char *server_conf_path = "config/server.conf";
    const char *resources_conf_path = "config/resources.conf";
    const char *cli_mode = NULL;
    int cli_port = -1;
    int cli_workers = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            server_conf_path = argv[++i];
        } else if (strcmp(argv[i], "--resources") == 0 && i + 1 < argc) {
            resources_conf_path = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            cli_mode = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cli_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            cli_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    server_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    // Initialize logger to stdout first
    log_init("stdout");

    if (config_load(server_conf_path, resources_conf_path, &ctx.cfg) != 0) {
        LOG_E("Failed to load configuration files");
        return 1;
    }

    // CLI overrides
    if (cli_mode) {
        if (strcasecmp(cli_mode, "banker") == 0) ctx.cfg.mode = MODE_BANKER;
        else if (strcasecmp(cli_mode, "naive") == 0) ctx.cfg.mode = MODE_NAIVE;
        else if (strcasecmp(cli_mode, "lockorder") == 0) ctx.cfg.mode = MODE_LOCKORDER;
        else if (strcasecmp(cli_mode, "detect") == 0) ctx.cfg.mode = MODE_DETECT;
        else {
            LOG_E("Unknown mode '%s'", cli_mode);
            return 1;
        }
    }
    if (cli_port > 0) ctx.cfg.port = cli_port;
    if (cli_workers > 0) ctx.cfg.workers = cli_workers;

    // Switch log file if configured
    if (strcmp(ctx.cfg.log_file, "stdout") != 0 && strlen(ctx.cfg.log_file) > 0) {
        log_init(ctx.cfg.log_file);
    }

    const char *mode_name = "banker";
    switch (ctx.cfg.mode) {
        case MODE_BANKER:    mode_name = "banker (avoidance)"; break;
        case MODE_NAIVE:     mode_name = "naive (none)"; break;
        case MODE_LOCKORDER: mode_name = "lockorder (prevention)"; break;
        case MODE_DETECT:    mode_name = "detect (recovery)"; break;
    }

    LOG_I("Starting SafeServe v1.0 on port %d", ctx.cfg.port);
    LOG_I("Deadlock strategy: %s | Admission mode: %s",
          mode_name, ctx.cfg.admit_mode == ADMIT_ATOMIC ? "atomic" : "incremental");
    LOG_I("Workers: %d | Queue capacity: %d | Docroot: %s",
          ctx.cfg.workers, ctx.cfg.queue_capacity, ctx.cfg.docroot);

    // Signals
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // Initialize subsystems
    metrics_init();

    if (rm_init(&ctx.cfg.resources, ctx.cfg.mode, ctx.cfg.admit_mode, ctx.cfg.admit_timeout_ms) != 0) {
        LOG_E("Failed to initialize Resource Manager");
        return 1;
    }

    if (queue_init(&ctx.queue, ctx.cfg.queue_capacity) != 0) {
        LOG_E("Failed to initialize job queue");
        rm_destroy();
        return 1;
    }

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
        queue_destroy(&ctx.queue);
        rm_destroy();
        return 1;
    }

    LOG_I("SafeServe listening for connections on 0.0.0.0:%d", ctx.cfg.port);

    // Main acceptor loop
    while (g_keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(ctx.listen_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0) {
            if (!g_keep_running) break;
            continue;
        }

        // Set receive timeout (5 seconds) to avoid slowloris hanging
        net_set_recv_timeout(client_fd, 5);

        job_t job;
        job.client_fd = client_fd;
        clock_gettime(CLOCK_MONOTONIC, &job.t_accept);

        // Try push to bounded queue
        if (!queue_try_push(&ctx.queue, job)) {
            // Queue full: reject immediately with 503 (FR-8)
            const char *err = "503 Service Unavailable (Queue Full)\n";
            http_send_response(client_fd, 503, "text/plain", err, strlen(err), 1);
            metrics_inc_queue_full();
            metrics_inc_status(503);
            close(client_fd);
        }
    }

    LOG_I("Shutting down SafeServe...");
    close(ctx.listen_fd);
    rm_shutdown();
    worker_pool_stop(&ctx);
    queue_destroy(&ctx.queue);
    rm_destroy();
    log_close();

    printf("SafeServe gracefully stopped.\n");
    return 0;
}
