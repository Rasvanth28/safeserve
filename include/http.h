#ifndef SAFESERVE_HTTP_H
#define SAFESERVE_HTTP_H

#include <stddef.h>
#include <stdbool.h>

#define HTTP_MAX_HEADER_SIZE 8192
#define HTTP_MAX_PATH_LEN 1024
#define HTTP_MAX_BODY_SIZE (1024 * 1024 * 10) // 10 MB

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_UNKNOWN
} http_method_t;

typedef struct {
    http_method_t method;
    char path[HTTP_MAX_PATH_LEN];
    char version[16];
    size_t content_length;
    bool keep_alive;
    char *body; // Points into buffer or dynamically allocated if body exists
    size_t body_len;
} http_request_t;

// Parses raw buffer into http_request_t.
// Returns 0 on success, -1 on invalid format, -2 on incomplete (need more data)
int http_parse_request(const char *raw_buf, size_t buf_len, http_request_t *req);

// Cleans up any resources associated with req
void http_free_request(http_request_t *req);

// Sanitizes url_path and verifies it resolves strictly inside docroot.
// Returns 0 on success (with resolved_path_out populated), -1 on traversal/not found
int http_sanitize_path(const char *docroot, const char *url_path, char *resolved_path_out, size_t max_len);

// Sends full HTTP response over socket
int http_send_response(int sock_fd, int status_code, const char *content_type,
                       const void *body, size_t body_len, int retry_after_sec);

// Sends HTTP response streaming a file
int http_send_file(int sock_fd, const char *content_type, int file_fd, size_t file_len);

// Helper for MIME types
const char *http_guess_content_type(const char *path);

// Helper for status code reason phrases
const char *http_status_reason(int status_code);

#endif // SAFESERVE_HTTP_H
