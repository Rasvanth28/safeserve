#ifndef SAFESERVE_HANDLERS_H
#define SAFESERVE_HANDLERS_H

#include "http.h"
#include "config.h"

#define METHOD_BIT(m) (1u << (m))

// A handler sends exactly one response and returns its HTTP status code.
// slot is the resource-manager slot (-1 for zero-claim routes).
typedef int (*handler_fn)(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);

typedef struct {
    const char *pattern;      // Exact path, or prefix ending in '*'
    const char *claim_path;   // Path used to look up the claim in resources.conf
    unsigned methods;         // Bitmask of METHOD_BIT(HTTP_METHOD_*)
    const char *allow;        // Value for the Allow header on 405
    handler_fn fn;
} route_t;

// Returns the route for a (decoded, query-less) path, or NULL.
const route_t *handlers_find_route(const char *path);

int handle_static(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_upload(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_query(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_refresh(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_health(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);
int handle_metrics(int sock_fd, const http_request_t *req, const server_config_t *cfg, int slot);

// Sends a plain-text error response and returns status_code.
int send_error(int sock_fd, int status_code, const char *detail);

// Maps an RM_* failure to the HTTP response and sends it. Returns the status.
int send_rm_failure(int sock_fd, int rm_rc);

#endif // SAFESERVE_HANDLERS_H
