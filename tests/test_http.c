#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void test_parse_get(void) {
    const char *raw = "GET /static/index.html HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUser-Agent: curl/7.79.1\r\n\r\n";
    http_request_t req;
    int rc = http_parse_request(raw, strlen(raw), &req);
    assert(rc == 0);
    assert(req.method == HTTP_METHOD_GET);
    assert(strcmp(req.path, "/static/index.html") == 0);
    assert(strcmp(req.version, "HTTP/1.1") == 0);
    assert(req.content_length == 0);
    assert(req.body == NULL);
    http_free_request(&req);
    printf("[test_http] GET request parsing: PASS\n");
}

static void test_parse_post(void) {
    const char *raw = "POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\nHello World";
    http_request_t req;
    int rc = http_parse_request(raw, strlen(raw), &req);
    assert(rc == 0);
    assert(req.method == HTTP_METHOD_POST);
    assert(strcmp(req.path, "/upload") == 0);
    assert(req.content_length == 11);
    assert(req.body != NULL);
    assert(strcmp(req.body, "Hello World") == 0);
    http_free_request(&req);
    printf("[test_http] POST request with body parsing: PASS\n");
}

static void test_incomplete_request(void) {
    const char *raw = "GET /query HTTP/1.1\r\nHost: localhost\r\n"; // Missing final \r\n
    http_request_t req;
    int rc = http_parse_request(raw, strlen(raw), &req);
    assert(rc == -2); // Need more data
    printf("[test_http] Incomplete header detection: PASS\n");
}

static void test_path_sanitization(void) {
    char out_path[1024];

    // Traversal attack
    int rc1 = http_sanitize_path("./www", "/static/../../etc/passwd", out_path, sizeof(out_path));
    assert(rc1 == -1);

    int rc2 = http_sanitize_path("./www", "/../secret", out_path, sizeof(out_path));
    assert(rc2 == -1);

    // Valid path resolving into www/index.html
    int rc3 = http_sanitize_path("./www", "/index.html", out_path, sizeof(out_path));
    assert(rc3 == 0);
    assert(strstr(out_path, "index.html") != NULL);

    printf("[test_http] Path sanitization and traversal prevention: PASS\n");
}

int main(void) {
    printf("=== Running HTTP Parser Unit Tests ===\n");
    test_parse_get();
    test_parse_post();
    test_incomplete_request();
    test_path_sanitization();
    printf("=== All HTTP Parser Tests Passed! ===\n");
    return 0;
}
