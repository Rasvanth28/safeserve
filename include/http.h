#ifndef SAFESERVE_HTTP_H
#define SAFESERVE_HTTP_H

#include <stddef.h>
#include <stdbool.h>

#define HTTP_MAX_HEADER_SIZE 8192
#define HTTP_MAX_PATH_LEN 1024
#define HTTP_MAX_BODY_SIZE (1024 * 1024 * 10) // 10 MB

// Parser results (negative) besides 0 = success
#define HTTP_PARSE_INVALID     -1   // Malformed request (400)
#define HTTP_PARSE_INCOMPLETE  -2   // Need more bytes
#define HTTP_PARSE_TOO_LARGE   -3   // Body larger than HTTP_MAX_BODY_SIZE (413)
#define HTTP_PARSE_UNSUPPORTED -4   // Transfer-Encoding etc. (501)

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_POST,
    HTTP_METHOD_UNKNOWN
} http_method_t;

typedef struct {
    http_method_t method;
    char path[HTTP_MAX_PATH_LEN];    // Percent-decoded path, without query string
    char query[HTTP_MAX_PATH_LEN];   // Raw query string after '?', or ""
    char version[16];
    size_t content_length;
    bool keep_alive;
    char *body;                      // Heap-allocated, NUL-terminated, or NULL
    size_t body_len;
} http_request_t;

// Parses a complete request held in raw_buf. Returns 0 or an HTTP_PARSE_* code.
int http_parse_request(const char *raw_buf, size_t buf_len, http_request_t *req);

// Reads one request from a socket, looping over recv() until the headers and
// the full Content-Length body have arrived.
// Returns 0 on success, -1 if the peer closed/reset before sending anything,
// or an HTTP status code to answer with (400, 408, 413, 431, 501).
int http_read_request(int sock_fd, http_request_t *req);

// Cleans up any resources associated with req
void http_free_request(http_request_t *req);

// Maps a URL path to a regular file under docroot. Rejects '..', backslashes
// and anything whose realpath() escapes docroot (e.g. via a symlink).
// Returns 0 with resolved_path_out set, or -1 if forbidden or not found.
int http_sanitize_path(const char *docroot, const char *url_path, char *resolved_path_out, size_t max_len);

// Sends a full HTTP response. retry_after_sec > 0 adds Retry-After (503).
int http_send_response(int sock_fd, int status_code, const char *content_type,
                       const void *body, size_t body_len, int retry_after_sec);

// Same, with extra raw header lines (each ending in "\r\n"), e.g. "Allow: GET\r\n".
int http_send_response_ex(int sock_fd, int status_code, const char *content_type,
                          const void *body, size_t body_len, const char *extra_headers);

// Sends HTTP response streaming a file. head_only skips the body (HEAD).
int http_send_file(int sock_fd, const char *content_type, int file_fd, size_t file_len, bool head_only);

// Helper for MIME types
const char *http_guess_content_type(const char *path);

// Helper for status code reason phrases
const char *http_status_reason(int status_code);

#endif // SAFESERVE_HTTP_H
