#ifndef SAFESERVE_HANDLERS_H
#define SAFESERVE_HANDLERS_H

#include "http.h"
#include "config.h"

// Dispatcher for incoming requests based on route
int handle_request(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);

int handle_static(int sock_fd, const http_request_t *req, const server_config_t *cfg);
int handle_upload(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_query(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_refresh(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_health(int sock_fd, const http_request_t *req);
int handle_metrics(int sock_fd, const http_request_t *req);

#endif // SAFESERVE_HANDLERS_H
