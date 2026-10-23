#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

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
    if (!rc || !path) return 0;

    // Check exact matches or wildcard matches (e.g. /static/*)
    for (int i = 0; i < rc->num_routes; i++) {
        const char *pat = rc->routes[i].pattern;
        size_t pat_len = strlen(pat);
        if (pat_len > 0 && pat[pat_len - 1] == '*') {
            if (strncmp(path, pat, pat_len - 1) == 0) {
                for (int j = 0; j < rc->m; j++) {
                    claim_out[j] = rc->routes[i].claim[j];
                }
                return 0;
            }
        } else {
            if (strcmp(path, pat) == 0) {
                for (int j = 0; j < rc->m; j++) {
                    claim_out[j] = rc->routes[i].claim[j];
                }
                return 0;
            }
        }
    }
    return 0;
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
            if (strcasecmp(val, "banker") == 0) cfg->mode = MODE_BANKER;
            else if (strcasecmp(val, "naive") == 0) cfg->mode = MODE_NAIVE;
            else if (strcasecmp(val, "lockorder") == 0) cfg->mode = MODE_LOCKORDER;
            else if (strcasecmp(val, "detect") == 0) cfg->mode = MODE_DETECT;
        } else if (strcasecmp(key, "admit_mode") == 0) {
            if (strcasecmp(val, "atomic") == 0) cfg->admit_mode = ADMIT_ATOMIC;
            else if (strcasecmp(val, "incremental") == 0) cfg->admit_mode = ADMIT_INCREMENTAL;
        } else if (strcasecmp(key, "admit_timeout_ms") == 0) {
            cfg->admit_timeout_ms = atoi(val);
        } else if (strcasecmp(key, "detect_interval_ms") == 0) {
            cfg->detect_interval_ms = atoi(val);
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
    cfg->detect_interval_ms = 200;
    strncpy(cfg->docroot, "./www", sizeof(cfg->docroot) - 1);
    strncpy(cfg->log_file, "stdout", sizeof(cfg->log_file) - 1);
    cfg->simulated_db_delay_ms = 50;
    cfg->simulated_upload_delay_ms = 30;
    cfg->simulated_refresh_delay_ms = 40;

    if (parse_server_conf(server_conf_path, cfg) != 0) {
        return -1;
    }
    if (parse_resources_conf(resources_conf_path, &cfg->resources) != 0) {
        return -1;
    }

    return 0;
}
