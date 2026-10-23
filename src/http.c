#define _XOPEN_SOURCE 700 // realpath() is an X/Open extension
#include "http.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <limits.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

const char *http_status_reason(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
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
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    return "application/octet-stream";
}

// Finds the blank line ending the header block. Sets *body_offset past it.
static const char *find_header_end(const char *buf, size_t len, size_t *body_offset) {
    for (size_t i = 0; i < len; i++) {
        if (i + 3 < len && memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            *body_offset = i + 4;
            return buf + i;
        }
        if (i + 1 < len && memcmp(buf + i, "\n\n", 2) == 0) {
            *body_offset = i + 2;
            return buf + i;
        }
    }
    return NULL;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decodes src into dst. Rejects malformed escapes and %00.
static int url_decode(const char *src, size_t src_len, char *dst, size_t dst_size) {
    size_t o = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (c == '%') {
            if (i + 2 >= src_len) return -1;
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (char)(hi * 16 + lo);
            if (c == '\0') return -1;
            i += 2;
        }
        if (o + 1 >= dst_size) return -1;
        dst[o++] = c;
    }
    dst[o] = '\0';
    return 0;
}

// Parses the request line and headers. Body is not touched.
static int parse_head(const char *raw_buf, size_t buf_len, http_request_t *req, size_t *body_offset) {
    memset(req, 0, sizeof(*req));

    const char *header_end = find_header_end(raw_buf, buf_len, body_offset);
    if (!header_end) return HTTP_PARSE_INCOMPLETE;

    // Request line
    // Search up to body_offset: with no headers the request line's own
    // newline is part of the blank-line terminator.
    const char *eol = memchr(raw_buf, '\n', *body_offset);
    if (!eol) return HTTP_PARSE_INVALID;
    size_t line_len = (size_t)(eol - raw_buf);
    if (line_len > 0 && raw_buf[line_len - 1] == '\r') line_len--;

    char line[HTTP_MAX_PATH_LEN + 64];
    if (line_len >= sizeof(line)) return HTTP_PARSE_INVALID;
    memcpy(line, raw_buf, line_len);
    line[line_len] = '\0';

    char method_str[16], target[HTTP_MAX_PATH_LEN], ver_str[16];
    char extra;
    if (sscanf(line, "%15s %1023s %15s %c", method_str, target, ver_str, &extra) != 3) {
        return HTTP_PARSE_INVALID;
    }
    if (strncmp(ver_str, "HTTP/1.", 7) != 0 || target[0] != '/') {
        return HTTP_PARSE_INVALID;
    }

    if (strcmp(method_str, "GET") == 0) req->method = HTTP_METHOD_GET;
    else if (strcmp(method_str, "HEAD") == 0) req->method = HTTP_METHOD_HEAD;
    else if (strcmp(method_str, "POST") == 0) req->method = HTTP_METHOD_POST;
    else req->method = HTTP_METHOD_UNKNOWN;

    size_t path_len = strcspn(target, "?#");
    if (url_decode(target, path_len, req->path, sizeof(req->path)) != 0) {
        return HTTP_PARSE_INVALID;
    }
    if (target[path_len] == '?') {
        size_t q_len = strcspn(target + path_len + 1, "#");
        memcpy(req->query, target + path_len + 1, q_len);
        req->query[q_len] = '\0';
    }
    memcpy(req->version, ver_str, sizeof(req->version)); // Both 16 bytes, NUL-terminated by sscanf
    req->keep_alive = false; // Every response is sent with Connection: close

    // Headers
    const char *curr = eol + 1;
    if (curr > header_end) curr = header_end;
    bool seen_length = false;
    while (curr < header_end) {
        const char *next_eol = memchr(curr, '\n', (size_t)(header_end - curr));
        if (!next_eol) next_eol = header_end;
        size_t hlen = (size_t)(next_eol - curr);
        if (hlen > 0 && curr[hlen - 1] == '\r') hlen--;

        if (hlen > 0) {
            if (hlen >= sizeof(line)) return HTTP_PARSE_INVALID;
            memcpy(line, curr, hlen);
            line[hlen] = '\0';

            char *colon = strchr(line, ':');
            if (!colon) return HTTP_PARSE_INVALID;
            *colon = '\0';
            char *hname = line;
            char *hval = colon + 1;
            while (*hval == ' ' || *hval == '\t') hval++;

            if (strcasecmp(hname, "Content-Length") == 0) {
                if (*hval == '\0') return HTTP_PARSE_INVALID;
                for (const char *p = hval; *p; p++) {
                    if (!isdigit((unsigned char)*p) && *p != ' ' && *p != '\t') {
                        return HTTP_PARSE_INVALID;
                    }
                }
                errno = 0;
                unsigned long long v = strtoull(hval, NULL, 10);
                if (errno != 0) return HTTP_PARSE_TOO_LARGE;
                if (seen_length && req->content_length != v) return HTTP_PARSE_INVALID;
                req->content_length = (size_t)v;
                seen_length = true;
            } else if (strcasecmp(hname, "Transfer-Encoding") == 0) {
                return HTTP_PARSE_UNSUPPORTED;
            }
        }
        curr = next_eol + 1;
    }

    if (req->content_length > HTTP_MAX_BODY_SIZE) return HTTP_PARSE_TOO_LARGE;
    return 0;
}

int http_parse_request(const char *raw_buf, size_t buf_len, http_request_t *req) {
    if (!raw_buf || !req) return HTTP_PARSE_INVALID;

    size_t body_offset = 0;
    int rc = parse_head(raw_buf, buf_len, req, &body_offset);
    if (rc != 0) return rc;

    if (req->content_length > 0) {
        size_t available_body = buf_len - body_offset;
        if (available_body < req->content_length) return HTTP_PARSE_INCOMPLETE;
        req->body = malloc(req->content_length + 1);
        if (!req->body) return HTTP_PARSE_INVALID;
        memcpy(req->body, raw_buf + body_offset, req->content_length);
        req->body[req->content_length] = '\0';
        req->body_len = req->content_length;
    }
    return 0;
}

static int parse_rc_to_status(int rc) {
    switch (rc) {
        case HTTP_PARSE_TOO_LARGE:   return 413;
        case HTTP_PARSE_UNSUPPORTED: return 501;
        default:                     return 400;
    }
}

int http_read_request(int sock_fd, http_request_t *req) {
    char head[HTTP_MAX_HEADER_SIZE];
    size_t len = 0;
    size_t body_offset = 0;
    memset(req, 0, sizeof(*req));

    // 1. Read until the header block is complete.
    while (true) {
        if (len == sizeof(head)) return 431;
        ssize_t n = recv(sock_fd, head + len, sizeof(head) - len, 0);
        if (n == 0) return len == 0 ? -1 : 400;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return len == 0 ? -1 : 408;
            return -1;
        }
        len += (size_t)n;
        if (find_header_end(head, len, &body_offset)) break;
    }

    int rc = parse_head(head, len, req, &body_offset);
    if (rc != 0) return parse_rc_to_status(rc);
    if (req->content_length == 0) return 0;

    // 2. Copy any body bytes that arrived with the headers, then read the rest.
    req->body = malloc(req->content_length + 1);
    if (!req->body) return 500;

    size_t have = len - body_offset;
    if (have > req->content_length) have = req->content_length;
    memcpy(req->body, head + body_offset, have);

    while (have < req->content_length) {
        ssize_t n = recv(sock_fd, req->body + have, req->content_length - have, 0);
        if (n == 0) return 400;
        if (n < 0) {
            if (errno == EINTR) continue;
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 408 : -1;
        }
        have += (size_t)n;
    }
    req->body[req->content_length] = '\0';
    req->body_len = req->content_length;
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

    // Reject traversal outright, then double-check with realpath() below.
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
    while (*subpath == '/') subpath++;

    char candidate[PATH_MAX];
    int n;
    if (*subpath == '\0') {
        n = snprintf(candidate, sizeof(candidate), "%s/index.html", docroot);
    } else {
        n = snprintf(candidate, sizeof(candidate), "%s/%s", docroot, subpath);
    }
    if (n < 0 || (size_t)n >= sizeof(candidate)) return -1;

    struct stat st;
    if (stat(candidate, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        size_t clen = strlen(candidate);
        if (clen + sizeof("/index.html") > sizeof(candidate)) return -1;
        strcat(candidate, "/index.html");
        if (stat(candidate, &st) != 0) return -1;
    }
    if (!S_ISREG(st.st_mode)) return -1;

    // The file must really live under docroot (no symlink escapes).
    char real_root[PATH_MAX], real_file[PATH_MAX];
    if (!realpath(docroot, real_root) || !realpath(candidate, real_file)) return -1;
    size_t root_len = strlen(real_root);
    if (strncmp(real_file, real_root, root_len) != 0 || real_file[root_len] != '/') {
        return -1;
    }

    if (strlen(real_file) >= max_len) return -1;
    strcpy(resolved_path_out, real_file);
    return 0;
}

static ssize_t send_all(int sock_fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t sent = send(sock_fd, ptr + total, len - total, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        total += (size_t)sent;
    }
    return (ssize_t)total;
}

int http_send_response_ex(int sock_fd, int status_code, const char *content_type,
                          const void *body, size_t body_len, const char *extra_headers) {
    char header_buf[1024];
    const char *ctype = content_type ? content_type : "text/plain; charset=utf-8";

    int hlen = snprintf(header_buf, sizeof(header_buf),
                        "HTTP/1.1 %d %s\r\n"
                        "Server: SafeServe/1.1\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "%s"
                        "\r\n",
                        status_code, http_status_reason(status_code), ctype, body_len,
                        extra_headers ? extra_headers : "");
    if (hlen < 0 || (size_t)hlen >= sizeof(header_buf)) return -1;

    if (send_all(sock_fd, header_buf, (size_t)hlen) < 0) return -1;
    if (body && body_len > 0) {
        if (send_all(sock_fd, body, body_len) < 0) return -1;
    }
    return 0;
}

int http_send_response(int sock_fd, int status_code, const char *content_type,
                       const void *body, size_t body_len, int retry_after_sec) {
    char extra[64] = "";
    if (status_code == 503 && retry_after_sec > 0) {
        snprintf(extra, sizeof(extra), "Retry-After: %d\r\n", retry_after_sec);
    }
    return http_send_response_ex(sock_fd, status_code, content_type, body, body_len, extra);
}

int http_send_file(int sock_fd, const char *content_type, int file_fd, size_t file_len, bool head_only) {
    if (http_send_response_ex(sock_fd, 200,
                              content_type ? content_type : "application/octet-stream",
                              NULL, file_len, NULL) != 0) {
        return -1;
    }
    if (head_only) return 0;

    char chunk[4096];
    ssize_t nread;
    while ((nread = read(file_fd, chunk, sizeof(chunk))) > 0) {
        if (send_all(sock_fd, chunk, (size_t)nread) < 0) {
            return -1;
        }
    }
    return nread < 0 ? -1 : 0;
}
