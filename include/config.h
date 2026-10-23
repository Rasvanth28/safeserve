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
    int admit_timeout_ms;
    int detect_interval_ms;
    char docroot[MAX_PATH_LEN];
    char log_file[MAX_PATH_LEN];
    int simulated_db_delay_ms;
    int simulated_upload_delay_ms;
    int simulated_refresh_delay_ms;
    resource_config_t resources;
} server_config_t;

// Parses server.conf and resources.conf into cfg
int config_load(const char *server_conf_path, const char *resources_conf_path, server_config_t *cfg);

// Helper to look up resource index by name (-1 if not found)
int config_get_resource_idx(const resource_config_t *rc, const char *name);

// Look up maximum claim vector for a route. Writes into claim_out.
// Returns 0 on success.
int config_get_route_claim(const resource_config_t *rc, const char *path, int claim_out[M_MAX]);

#endif // SAFESERVE_CONFIG_H
