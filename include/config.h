#ifndef SAFESERVE_CONFIG_H
#define SAFESERVE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define M_MAX 8
#define N_MAX 1024
#define MAX_ROUTES 32
#define MAX_PATH_LEN 512

typedef enum {
    MODE_BANKER = 0,
    MODE_NAIVE = 1,
    MODE_LOCKORDER = 2,
    MODE_DETECT = 3
} server_mode_t;

typedef enum {
    ADMIT_ATOMIC = 0,
    ADMIT_INCREMENTAL = 1
} admit_mode_t;

// One step of an incremental acquisition plan: `count` units of resource `res`.
typedef struct {
    int res;
    int count;
} acq_step_t;

typedef struct {
    char pattern[64];
    int claim[M_MAX];
} route_claim_t;

typedef struct {
    int m;                               // Number of resource types
    char res_names[M_MAX][32];           // Resource names (e.g. WORKER, BUF, FD, DB, CLOCK)
    int total[M_MAX];                    // Total capacity per resource
    int num_routes;
    route_claim_t routes[MAX_ROUTES];
} resource_config_t;

typedef struct {
    int port;
    int workers;
    int queue_capacity;
    server_mode_t mode;
    admit_mode_t admit_mode;
    int admit_timeout_ms;                // Total admission wait allowed per request
    int queue_timeout_ms;                // Max time in the accept queue before 503
    int detect_interval_ms;
    int fairness_aging_ms;               // banker starvation guard (0 = off)
    char docroot[MAX_PATH_LEN];
    char log_file[MAX_PATH_LEN];
    int simulated_db_delay_ms;
    int simulated_upload_delay_ms;
    int simulated_refresh_delay_ms;
    int step_delay_ms;                   // Pause between incremental acquisition steps
    char util_csv[MAX_PATH_LEN];         // Resource-utilization CSV ("" = disabled)
    int util_interval_ms;
    resource_config_t resources;
} server_config_t;

// Parses server.conf and resources.conf into cfg
int config_load(const char *server_conf_path, const char *resources_conf_path, server_config_t *cfg);

// Helper to look up resource index by name (-1 if not found)
int config_get_resource_idx(const resource_config_t *rc, const char *name);

// Look up the maximum claim vector for a route path (query string ignored).
// Writes zeros and returns -1 if no route pattern matches; returns 0 otherwise.
int config_get_route_claim(const resource_config_t *rc, const char *path, int claim_out[M_MAX]);

// Rejects configurations Banker's cannot serve: a route claim larger than the
// total of some resource (FR-15). Returns 0 if valid, logs and returns -1 if not.
int config_validate(const server_config_t *cfg);

// Name <-> enum helpers shared by the config parser and the CLI.
// Return 0 on success, -1 on an unknown name.
int config_parse_mode(const char *name, server_mode_t *out);
int config_parse_admit_mode(const char *name, admit_mode_t *out);
const char *config_mode_name(server_mode_t mode);
const char *config_admit_mode_name(admit_mode_t mode);

#endif // SAFESERVE_CONFIG_H
