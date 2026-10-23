#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE // mkdtemp() on macOS
#include "http.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>

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

static void test_query_and_decoding(void) {
    http_request_t req;
    const char *raw = "GET /query?id=7&x=%41 HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(http_parse_request(raw, strlen(raw), &req) == 0);
    assert(strcmp(req.path, "/query") == 0);
    assert(strcmp(req.query, "id=7&x=%41") == 0);

    const char *enc = "GET /static/a%20b.txt HTTP/1.1\r\n\r\n";
    assert(http_parse_request(enc, strlen(enc), &req) == 0);
    assert(strcmp(req.path, "/static/a b.txt") == 0);

    // Encoded traversal decodes to '..' and is then refused by sanitization
    const char *trav = "GET /static/%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\n\r\n";
    assert(http_parse_request(trav, strlen(trav), &req) == 0);
    char out[1024];
    assert(http_sanitize_path("./www", req.path, out, sizeof(out)) == -1);

    const char *nul = "GET /static/a%00b HTTP/1.1\r\n\r\n";
    assert(http_parse_request(nul, strlen(nul), &req) == HTTP_PARSE_INVALID);
    const char *bad = "GET /static/a%zz HTTP/1.1\r\n\r\n";
    assert(http_parse_request(bad, strlen(bad), &req) == HTTP_PARSE_INVALID);
    printf("[test_http] Query string split, percent-decoding, encoded traversal: PASS\n");
}

static void test_malformed(void) {
    http_request_t req;
    const char *cases[] = {
        "GARBAGE\r\n\r\n",
        "GET\r\n\r\n",
        "GET noslash HTTP/1.1\r\n\r\n",
        "GET / HTTP/2.0\r\n\r\n",
        "GET / HTTP/1.1 extra\r\n\r\n",
        "GET / HTTP/1.1\r\nNoColonHeader\r\n\r\n",
        "POST /upload HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
        "POST /upload HTTP/1.1\r\nContent-Length: -5\r\n\r\n",
        "POST /upload HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = http_parse_request(cases[i], strlen(cases[i]), &req);
        if (rc != HTTP_PARSE_INVALID) printf("case %zu -> %d\n", i, rc);
        assert(rc == HTTP_PARSE_INVALID);
    }

    const char *huge = "POST /upload HTTP/1.1\r\nContent-Length: 999999999999\r\n\r\n";
    assert(http_parse_request(huge, strlen(huge), &req) == HTTP_PARSE_TOO_LARGE);
    const char *chunked = "POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    assert(http_parse_request(chunked, strlen(chunked), &req) == HTTP_PARSE_UNSUPPORTED);
    const char *partial_body = "POST /upload HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";
    assert(http_parse_request(partial_body, strlen(partial_body), &req) == HTTP_PARSE_INCOMPLETE);

    const char *head = "HEAD / HTTP/1.1\r\n\r\n";
    assert(http_parse_request(head, strlen(head), &req) == 0 && req.method == HTTP_METHOD_HEAD);
    const char *del = "DELETE / HTTP/1.1\r\n\r\n";
    assert(http_parse_request(del, strlen(del), &req) == 0 && req.method == HTTP_METHOD_UNKNOWN);
    printf("[test_http] Malformed lines, bad lengths, 413/501 cases, methods: PASS\n");
}

static void test_symlink_escape(void) {
    // A symlink inside docroot pointing outside it must not be served.
    char root[] = "/tmp/safeserve_test_XXXXXX";
    assert(mkdtemp(root) != NULL);
    char link_path[PATH_MAX], file_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s/escape", root);
    snprintf(file_path, sizeof(file_path), "%s/ok.txt", root);
    assert(symlink("/etc/hosts", link_path) == 0);
    FILE *f = fopen(file_path, "w");
    assert(f);
    fputs("ok", f);
    fclose(f);

    char out[PATH_MAX];
    assert(http_sanitize_path(root, "/static/escape", out, sizeof(out)) == -1);
    assert(http_sanitize_path(root, "/static/ok.txt", out, sizeof(out)) == 0);

    unlink(link_path);
    unlink(file_path);
    rmdir(root);
    printf("[test_http] Symlink escaping docroot refused (realpath check): PASS\n");
}

int main(void) {
    printf("=== Running HTTP Parser Unit Tests ===\n");
    test_parse_get();
    test_parse_post();
    test_incomplete_request();
    test_path_sanitization();
    test_query_and_decoding();
    test_malformed();
    test_symlink_escape();
    printf("=== All HTTP Parser Tests Passed! ===\n");
    return 0;
}
