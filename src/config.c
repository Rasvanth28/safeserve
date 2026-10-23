#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>

static char *trim_whitespace(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static void strip_comments(char *line) {
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char *semi = strchr(line, ';');
    if (semi) *semi = '\0';
}

int config_get_resource_idx(const resource_config_t *rc, const char *name) {
    if (!rc || !name) return -1;
    for (int i = 0; i < rc->m; i++) {
        if (strcasecmp(rc->res_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

int config_get_route_claim(const resource_config_t *rc, const char *path, int claim_out[M_MAX]) {
    for (int i = 0; i < M_MAX; i++) {
        claim_out[i] = 0;
    }
    if (!rc || !path) return -1;

    // Match on the path only: "/query?id=1" must get the /query claim.
    size_t path_len = strcspn(path, "?#");

    // Exact matches or prefix wildcards (e.g. /static/*)
    for (int i = 0; i < rc->num_routes; i++) {
        const char *pat = rc->routes[i].pattern;
        size_t pat_len = strlen(pat);
        bool match;
        if (pat_len > 0 && pat[pat_len - 1] == '*') {
            match = path_len >= pat_len - 1 && strncmp(path, pat, pat_len - 1) == 0;
        } else {
            match = path_len == pat_len && strncmp(path, pat, pat_len) == 0;
        }
        if (match) {
            for (int j = 0; j < rc->m; j++) {
                claim_out[j] = rc->routes[i].claim[j];
            }
            return 0;
        }
    }
    return -1;
}

int config_validate(const server_config_t *cfg) {
    const resource_config_t *rc = &cfg->resources;
    int rc_ok = 0;
    if (rc->m == 0) {
        LOG_E("resources.conf defines no resource types");
        return -1;
    }
    for (int i = 0; i < rc->num_routes; i++) {
        for (int j = 0; j < rc->m; j++) {
            if (rc->routes[i].claim[j] < 0 || rc->routes[i].claim[j] > rc->total[j]) {
                LOG_E("Route %s claims %d %s but only %d exist",
                      rc->routes[i].pattern, rc->routes[i].claim[j],
                      rc->res_names[j], rc->total[j]);
                rc_ok = -1;
            }
        }
    }
    if (cfg->workers <= 0 || cfg->queue_capacity <= 0) {
        LOG_E("workers and queue_capacity must be positive");
        rc_ok = -1;
    }
    return rc_ok;
}

int config_parse_mode(const char *name, server_mode_t *out) {
    if (strcasecmp(name, "banker") == 0) *out = MODE_BANKER;
    else if (strcasecmp(name, "naive") == 0) *out = MODE_NAIVE;
    else if (strcasecmp(name, "lockorder") == 0) *out = MODE_LOCKORDER;
    else if (strcasecmp(name, "detect") == 0) *out = MODE_DETECT;
    else return -1;
    return 0;
}

int config_parse_admit_mode(const char *name, admit_mode_t *out) {
    if (strcasecmp(name, "atomic") == 0) *out = ADMIT_ATOMIC;
    else if (strcasecmp(name, "incremental") == 0) *out = ADMIT_INCREMENTAL;
    else return -1;
    return 0;
}

const char *config_mode_name(server_mode_t mode) {
    switch (mode) {
        case MODE_BANKER:    return "banker";
        case MODE_NAIVE:     return "naive";
        case MODE_LOCKORDER: return "lockorder";
        case MODE_DETECT:    return "detect";
    }
    return "unknown";
}

const char *config_admit_mode_name(admit_mode_t mode) {
    return mode == ADMIT_ATOMIC ? "atomic" : "incremental";
}

static int parse_server_conf(const char *path, server_config_t *cfg) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_E("Failed to open server config: %s", path);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        strip_comments(line);
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '[') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *val = trim_whitespace(eq + 1);

        if (strcasecmp(key, "port") == 0) {
            cfg->port = atoi(val);
        } else if (strcasecmp(key, "workers") == 0) {
            cfg->workers = atoi(val);
        } else if (strcasecmp(key, "queue_capacity") == 0) {
            cfg->queue_capacity = atoi(val);
        } else if (strcasecmp(key, "mode") == 0) {
            if (config_parse_mode(val, &cfg->mode) != 0) {
                LOG_E("Unknown mode '%s' in %s", val, path);
                fclose(fp);
                return -1;
            }
        } else if (strcasecmp(key, "admit_mode") == 0) {
            if (config_parse_admit_mode(val, &cfg->admit_mode) != 0) {
                LOG_E("Unknown admit_mode '%s' in %s", val, path);
                fclose(fp);
                return -1;
            }
        } else if (strcasecmp(key, "admit_timeout_ms") == 0) {
            cfg->admit_timeout_ms = atoi(val);
        } else if (strcasecmp(key, "queue_timeout_ms") == 0) {
            cfg->queue_timeout_ms = atoi(val);
        } else if (strcasecmp(key, "detect_interval_ms") == 0) {
            cfg->detect_interval_ms = atoi(val);
        } else if (strcasecmp(key, "fairness_aging_ms") == 0) {
            cfg->fairness_aging_ms = atoi(val);
        } else if (strcasecmp(key, "docroot") == 0) {
            strncpy(cfg->docroot, val, sizeof(cfg->docroot) - 1);
        } else if (strcasecmp(key, "log_file") == 0) {
            strncpy(cfg->log_file, val, sizeof(cfg->log_file) - 1);
        } else if (strcasecmp(key, "simulated_db_delay_ms") == 0) {
            cfg->simulated_db_delay_ms = atoi(val);
        } else if (strcasecmp(key, "simulated_upload_delay_ms") == 0) {
            cfg->simulated_upload_delay_ms = atoi(val);
        } else if (strcasecmp(key, "simulated_refresh_delay_ms") == 0) {
            cfg->simulated_refresh_delay_ms = atoi(val);
        } else if (strcasecmp(key, "step_delay_ms") == 0) {
            cfg->step_delay_ms = atoi(val);
        } else if (strcasecmp(key, "util_csv") == 0) {
            strncpy(cfg->util_csv, val, sizeof(cfg->util_csv) - 1);
        } else if (strcasecmp(key, "util_interval_ms") == 0) {
            cfg->util_interval_ms = atoi(val);
        } else {
            LOG_W("Ignoring unknown key '%s' in %s", key, path);
        }
    }
    fclose(fp);
    return 0;
}

static int parse_resources_conf(const char *path, resource_config_t *rc) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_E("Failed to open resources config: %s", path);
        return -1;
    }

    char line[512];
    char current_section[128] = "";

    while (fgets(line, sizeof(line), fp)) {
        strip_comments(line);
        char *trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0') continue;

        if (trimmed[0] == '[' && trimmed[strlen(trimmed) - 1] == ']') {
            trimmed[strlen(trimmed) - 1] = '\0';
            strncpy(current_section, trim_whitespace(trimmed + 1), sizeof(current_section) - 1);
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *val = trim_whitespace(eq + 1);

        if (strcasecmp(current_section, "resources") == 0) {
            if (rc->m < M_MAX) {
                int idx = rc->m++;
                strncpy(rc->res_names[idx], key, sizeof(rc->res_names[idx]) - 1);
                rc->total[idx] = atoi(val);
            }
        } else if (strncasecmp(current_section, "route ", 6) == 0) {
            const char *route_pat = trim_whitespace(current_section + 6);
            // Find or create route entry
            int r_idx = -1;
            for (int i = 0; i < rc->num_routes; i++) {
                if (strcmp(rc->routes[i].pattern, route_pat) == 0) {
                    r_idx = i;
                    break;
                }
            }
            if (r_idx == -1 && rc->num_routes < MAX_ROUTES) {
                r_idx = rc->num_routes++;
                strncpy(rc->routes[r_idx].pattern, route_pat, sizeof(rc->routes[r_idx].pattern) - 1);
                for (int j = 0; j < M_MAX; j++) {
                    rc->routes[r_idx].claim[j] = 0;
                }
            }
            if (r_idx != -1) {
                int res_idx = config_get_resource_idx(rc, key);
                if (res_idx >= 0 && res_idx < M_MAX) {
                    rc->routes[r_idx].claim[res_idx] = atoi(val);
                } else {
                    LOG_W("Route %s: unknown resource '%s' (declare it in [resources] first)",
                          route_pat, key);
                }
            }
        }
    }

    fclose(fp);
    return 0;
}

int config_load(const char *server_conf_path, const char *resources_conf_path, server_config_t *cfg) {
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    // Default configuration values
    cfg->port = 8080;
    cfg->workers = 16;
    cfg->queue_capacity = 256;
    cfg->mode = MODE_BANKER;
    cfg->admit_mode = ADMIT_ATOMIC;
    cfg->admit_timeout_ms = 2000;
    cfg->queue_timeout_ms = 1000;
    cfg->detect_interval_ms = 200;
    cfg->fairness_aging_ms = 250;
    strncpy(cfg->docroot, "./www", sizeof(cfg->docroot) - 1);
    strncpy(cfg->log_file, "stdout", sizeof(cfg->log_file) - 1);
    cfg->simulated_db_delay_ms = 50;
    cfg->simulated_upload_delay_ms = 30;
    cfg->simulated_refresh_delay_ms = 40;
    cfg->step_delay_ms = 50;
    cfg->util_interval_ms = 100;

    if (parse_server_conf(server_conf_path, cfg) != 0) {
        return -1;
    }
    if (parse_resources_conf(resources_conf_path, &cfg->resources) != 0) {
        return -1;
    }

    return 0;
}
