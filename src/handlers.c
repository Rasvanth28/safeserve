#define _POSIX_C_SOURCE 200809L
#include "handlers.h"
#include "rm.h"
#include "metrics.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int handle_health(int sock_fd, const http_request_t *req) {
    (void)req;
    const char *resp = "{\"status\":\"ok\"}\n";
    return http_send_response(sock_fd, 200, "application/json", resp, strlen(resp), 0);
}

int handle_metrics(int sock_fd, const http_request_t *req) {
    (void)req;
    char report[4096];
    metrics_generate_report(report, sizeof(report));
    return http_send_response(sock_fd, 200, "application/json", report, strlen(report), 0);
}

int handle_static(int sock_fd, const http_request_t *req, const server_config_t *cfg) {
    char filepath[MAX_PATH_LEN];
    if (http_sanitize_path(cfg->docroot, req->path, filepath, sizeof(filepath)) != 0) {
        const char *err = "404 Not Found\n";
        return http_send_response(sock_fd, 404, "text/plain", err, strlen(err), 0);
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        const char *err = "404 Not Found\n";
        return http_send_response(sock_fd, 404, "text/plain", err, strlen(err), 0);
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        const char *err = "404 Not Found\n";
        return http_send_response(sock_fd, 404, "text/plain", err, strlen(err), 0);
    }

    const char *ctype = http_guess_content_type(filepath);
    int rc = http_send_file(sock_fd, ctype, fd, st.st_size);
    close(fd);
    return rc;
}

int handle_query(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    (void)req;
    (void)slot;
    if (cfg->simulated_db_delay_ms > 0) {
        usleep(cfg->simulated_db_delay_ms * 1000);
    }

    const char *resp = "{\"route\":\"/query\",\"status\":\"success\",\"data\":\"simulated db record\"}\n";
    return http_send_response(sock_fd, 200, "application/json", resp, strlen(resp), 0);
}

int handle_upload(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    (void)slot;
    if (cfg->mode == MODE_NAIVE && cfg->admit_mode == ADMIT_INCREMENTAL) {
        // Naive deadlock demo step: acquire CLOCK then sleep then DB
        int clock_idx = config_get_resource_idx(&cfg->resources, "CLOCK");
        int db_idx = config_get_resource_idx(&cfg->resources, "DB");
        if (clock_idx >= 0) rm_acquire(slot, clock_idx, 1, 0);
        usleep(50000); // 50ms artificial delay to trigger circular wait
        if (db_idx >= 0) rm_acquire(slot, db_idx, 1, 0);
    }

    if (cfg->simulated_upload_delay_ms > 0) {
        usleep(cfg->simulated_upload_delay_ms * 1000);
    }

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"route\":\"/upload\",\"status\":\"success\",\"bytes_received\":%zu}\n",
             req->body_len);
    return http_send_response(sock_fd, 200, "application/json", resp, strlen(resp), 0);
}

int handle_refresh(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    (void)req;
    if (cfg->mode == MODE_NAIVE && cfg->admit_mode == ADMIT_INCREMENTAL) {
        // Naive deadlock demo step: acquire DB then sleep then CLOCK (opposite of upload)
        int db_idx = config_get_resource_idx(&cfg->resources, "DB");
        int clock_idx = config_get_resource_idx(&cfg->resources, "CLOCK");
        if (db_idx >= 0) rm_acquire(slot, db_idx, 1, 0);
        usleep(50000); // 50ms artificial delay to trigger circular wait
        if (clock_idx >= 0) rm_acquire(slot, clock_idx, 1, 0);
    }

    if (cfg->simulated_refresh_delay_ms > 0) {
        usleep(cfg->simulated_refresh_delay_ms * 1000);
    }

    const char *resp = "{\"route\":\"/refresh\",\"status\":\"success\",\"refreshed\":true}\n";
    return http_send_response(sock_fd, 200, "application/json", resp, strlen(resp), 0);
}

int handle_request(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot) {
    if (strcmp(req->path, "/health") == 0) {
        return handle_health(sock_fd, req);
    }
    if (strcmp(req->path, "/metrics") == 0) {
        return handle_metrics(sock_fd, req);
    }
    if (strncmp(req->path, "/static", 7) == 0 || strcmp(req->path, "/") == 0) {
        return handle_static(sock_fd, req, cfg);
    }
    if (strncmp(req->path, "/query", 6) == 0) {
        return handle_query(sock_fd, req, cfg, slot);
    }
    if (strcmp(req->path, "/upload") == 0) {
        return handle_upload(sock_fd, req, cfg, slot);
    }
    if (strcmp(req->path, "/refresh") == 0) {
        return handle_refresh(sock_fd, req, cfg, slot);
    }

    const char *not_found = "404 Not Found\n";
    return http_send_response(sock_fd, 404, "text/plain", not_found, strlen(not_found), 0);
}
