#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

const char *http_status_reason(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

const char *http_guess_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(dot, ".css") == 0) return "text/css";
    if (strcasecmp(dot, ".js") == 0)  return "application/javascript";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    return "application/octet-stream";
}

int http_parse_request(const char *raw_buf, size_t buf_len, http_request_t *req) {
    if (!raw_buf || !req) return -1;
    memset(req, 0, sizeof(*req));

    const char *header_end = strstr(raw_buf, "\r\n\r\n");
    if (!header_end) {
        header_end = strstr(raw_buf, "\n\n");
        if (!header_end) return -2; // Incomplete
    }

    size_t header_len = (header_end - raw_buf);
    size_t body_offset = (strncmp(header_end, "\r\n\r\n", 4) == 0) ? (header_len + 4) : (header_len + 2);

    // Parse request line
    char line[1024];
    const char *eol = strstr(raw_buf, "\r\n");
    if (!eol) eol = strchr(raw_buf, '\n');
    if (!eol || (size_t)(eol - raw_buf) >= sizeof(line)) return -1;

    size_t line_len = eol - raw_buf;
    memcpy(line, raw_buf, line_len);
    line[line_len] = '\0';

    char method_str[16], path_str[HTTP_MAX_PATH_LEN], ver_str[16];
    if (sscanf(line, "%15s %1023s %15s", method_str, path_str, ver_str) != 3) {
        return -1;
    }

    if (strcasecmp(method_str, "GET") == 0) req->method = HTTP_METHOD_GET;
    else if (strcasecmp(method_str, "POST") == 0) req->method = HTTP_METHOD_POST;
    else req->method = HTTP_METHOD_UNKNOWN;

    strncpy(req->path, path_str, sizeof(req->path) - 1);
    strncpy(req->version, ver_str, sizeof(req->version) - 1);
    req->keep_alive = false; // By default deferred / close

    // Parse headers (scan for Content-Length)
    const char *curr = eol + ((strncmp(eol, "\r\n", 2) == 0) ? 2 : 1);
    while (curr < header_end) {
        const char *next_eol = strstr(curr, "\r\n");
        if (!next_eol) next_eol = strchr(curr, '\n');
        if (!next_eol || next_eol > header_end) next_eol = header_end;

        size_t hlen = next_eol - curr;
        if (hlen > 0 && hlen < sizeof(line)) {
            memcpy(line, curr, hlen);
            line[hlen] = '\0';

            char *colon = strchr(line, ':');
            if (colon) {
                *colon = '\0';
                char *hname = line;
                char *hval = colon + 1;
                while (*hval == ' ' || *hval == '\t') hval++;

                if (strcasecmp(hname, "Content-Length") == 0) {
                    req->content_length = (size_t)atoll(hval);
                }
            }
        }
        curr = next_eol + ((strncmp(next_eol, "\r\n", 2) == 0) ? 2 : 1);
    }

    // Body handling
    size_t available_body = (buf_len >= body_offset) ? (buf_len - body_offset) : 0;
    if (req->content_length > 0) {
        if (req->content_length > HTTP_MAX_BODY_SIZE) {
            return -1; // Payload too large
        }
        if (available_body < req->content_length) {
            return -2; // Incomplete body
        }
        req->body = malloc(req->content_length + 1);
        if (!req->body) return -1;
        memcpy(req->body, raw_buf + body_offset, req->content_length);
        req->body[req->content_length] = '\0';
        req->body_len = req->content_length;
    }

    return 0;
}

void http_free_request(http_request_t *req) {
    if (req && req->body) {
        free(req->body);
        req->body = NULL;
        req->body_len = 0;
    }
}

int http_sanitize_path(const char *docroot, const char *url_path, char *resolved_path_out, size_t max_len) {
    if (!docroot || !url_path || !resolved_path_out) return -1;

    // Strict traversal attack prevention: reject any '..' or backslash
    if (strstr(url_path, "..") != NULL || strchr(url_path, '\\') != NULL) {
        return -1;
    }

    // Strip leading /static if present
    const char *subpath = url_path;
    if (strncmp(subpath, "/static/", 8) == 0) {
        subpath += 7;
    } else if (strcmp(subpath, "/static") == 0) {
        subpath = "/";
    }

    // Skip leading slashes in subpath
    while (*subpath == '/') subpath++;

    char candidate[PATH_MAX];
    if (strlen(subpath) == 0) {
        snprintf(candidate, sizeof(candidate), "%s/index.html", docroot);
    } else {
        snprintf(candidate, sizeof(candidate), "%s/%s", docroot, subpath);
    }

    struct stat st;
    if (stat(candidate, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            char candidate_index[PATH_MAX];
            snprintf(candidate_index, sizeof(candidate_index), "%s/index.html", candidate);
            if (stat(candidate_index, &st) == 0 && S_ISREG(st.st_mode)) {
                strncpy(resolved_path_out, candidate_index, max_len - 1);
                resolved_path_out[max_len - 1] = '\0';
                return 0;
            }
            return -1;
        } else if (S_ISREG(st.st_mode)) {
            strncpy(resolved_path_out, candidate, max_len - 1);
            resolved_path_out[max_len - 1] = '\0';
            return 0;
        }
    }

    return -1;
}

static ssize_t send_all(int sock_fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(sock_fd, ptr + total, len - total, 0);
        if (sent <= 0) return sent;
        total += sent;
    }
    return (ssize_t)total;
}

int http_send_response(int sock_fd, int status_code, const char *content_type,
                       const void *body, size_t body_len, int retry_after_sec) {
    char header_buf[1024];
    int hlen;

    const char *reason = http_status_reason(status_code);
    const char *ctype = content_type ? content_type : "text/plain; charset=utf-8";

    if (status_code == 503 && retry_after_sec > 0) {
        hlen = snprintf(header_buf, sizeof(header_buf),
                        "HTTP/1.1 %d %s\r\n"
                        "Server: SafeServe/1.0\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "Retry-After: %d\r\n"
                        "\r\n",
                        status_code, reason, ctype, body_len, retry_after_sec);
    } else {
        hlen = snprintf(header_buf, sizeof(header_buf),
                        "HTTP/1.1 %d %s\r\n"
                        "Server: SafeServe/1.0\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status_code, reason, ctype, body_len);
    }

    if (hlen < 0 || (size_t)hlen >= sizeof(header_buf)) return -1;

    if (send_all(sock_fd, header_buf, hlen) <= 0) return -1;
    if (body && body_len > 0) {
        if (send_all(sock_fd, body, body_len) <= 0) return -1;
    }
    return 0;
}

int http_send_file(int sock_fd, const char *content_type, int file_fd, size_t file_len) {
    char header_buf[1024];
    int hlen = snprintf(header_buf, sizeof(header_buf),
                        "HTTP/1.1 200 OK\r\n"
                        "Server: SafeServe/1.0\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        content_type ? content_type : "application/octet-stream",
                        file_len);

    if (hlen < 0 || (size_t)hlen >= sizeof(header_buf)) return -1;
    if (send_all(sock_fd, header_buf, hlen) <= 0) return -1;

    char chunk[4096];
    ssize_t nread;
    while ((nread = read(file_fd, chunk, sizeof(chunk))) > 0) {
        if (send_all(sock_fd, chunk, nread) <= 0) {
            return -1;
        }
    }
    return 0;
}
