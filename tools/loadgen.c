#define _POSIX_C_SOURCE 200809L
// SafeServe load generator.
//
// Closed-loop HTTP/1.1 clients: each client thread sends one request, waits for
// the full response (or a timeout), records it, and repeats. Routes are drawn
// from a workload mix with a per-thread seeded RNG, so runs are reproducible.
//
//   loadgen --port 8080 --clients 100 --duration 10 --warmup 2
//           --mix balanced --seed 1 --label banker --csv results/bench.csv
//
// A request that is not fully answered within --timeout-ms (connect, send and
// response together) counts as "timeout": with a deadlocked server those pile
// up while goodput drops to zero. Latency percentiles cover 2xx responses only.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef struct {
    const char *name;
    int weight[4]; // static, query, upload, refresh (percent)
} mix_t;

static const mix_t mixes[] = {
    { "static",   { 90, 6, 2, 2 } },
    { "balanced", { 40, 30, 15, 15 } },
    { "write",    { 10, 30, 30, 30 } },
};

static const char *route_req[4] = {
    "GET /static/index.html HTTP/1.1\r\nHost: loadgen\r\nConnection: close\r\n\r\n",
    "GET /query?id=%u HTTP/1.1\r\nHost: loadgen\r\nConnection: close\r\n\r\n",
    "POST /upload HTTP/1.1\r\nHost: loadgen\r\nConnection: close\r\nContent-Length: %d\r\n\r\n",
    "POST /refresh HTTP/1.1\r\nHost: loadgen\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
};

#define UPLOAD_BYTES 1024

typedef struct {
    double *lat;
    size_t n, cap;
    uint64_t ok, s503, other, timeouts, conn_err;
    uint64_t per_route_ok[4];
} stats_t;

static struct {
    const char *host;
    int port;
    int clients;
    double duration_s;
    double warmup_s;
    int timeout_ms;
    const mix_t *mix;
    unsigned int seed;
    const char *label;
    const char *csv;
} opt = { "127.0.0.1", 8080, 50, 10.0, 2.0, 3000, &mixes[1], 1, "run", NULL };

static struct timespec t_begin;
static atomic_bool stop_flag;
static stats_t *g_stats;

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t_begin.tv_sec) + (double)(t.tv_nsec - t_begin.tv_nsec) / 1e9;
}

static void record(stats_t *s, double ms) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->lat = realloc(s->lat, s->cap * sizeof(double));
        if (!s->lat) { perror("realloc"); exit(1); }
    }
    s->lat[s->n++] = ms;
}

typedef enum { RES_OK, RES_503, RES_OTHER, RES_TIMEOUT, RES_CONN } result_t;

// Waits for `events` on fd until the absolute deadline. Returns 1 if ready,
// 0 on timeout, -1 on error.
static int wait_fd(int fd, short events, double deadline) {
    while (true) {
        int ms = (int)((deadline - now_s()) * 1000.0);
        if (ms <= 0) return 0;
        struct pollfd p = { fd, events, 0 };
        int r = poll(&p, 1, ms);
        if (r > 0) return 1;
        if (r == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

// One request with a single deadline covering connect, send and the whole
// response (a per-recv timeout would let slow trickles run far past it).
static result_t one_request(int route, unsigned int *seed) {
    double deadline = now_s() + opt.timeout_ms / 1000.0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return RES_CONN;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)opt.port);
    inet_pton(AF_INET, opt.host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) { close(fd); return RES_CONN; }
        int w = wait_fd(fd, POLLOUT, deadline);
        if (w <= 0) { close(fd); return w == 0 ? RES_TIMEOUT : RES_CONN; }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) { close(fd); return RES_CONN; }
    }

    char req[512 + UPLOAD_BYTES];
    int len;
    if (route == 1) {
        len = snprintf(req, sizeof(req), route_req[1], (unsigned)rand_r(seed) % 10000);
    } else if (route == 2) {
        len = snprintf(req, sizeof(req), route_req[2], UPLOAD_BYTES);
        memset(req + len, 'x', UPLOAD_BYTES);
        len += UPLOAD_BYTES;
    } else {
        len = snprintf(req, sizeof(req), "%s", route_req[route]);
    }

    for (int sent = 0; sent < len;) {
        ssize_t n = send(fd, req + sent, (size_t)(len - sent), 0);
        if (n > 0) { sent += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            int w = wait_fd(fd, POLLOUT, deadline);
            if (w <= 0) { close(fd); return w == 0 ? RES_TIMEOUT : RES_CONN; }
            continue;
        }
        close(fd);
        return RES_CONN;
    }

    // Read the whole response (the server always closes the connection).
    char buf[4096];
    char status_line[16] = "";
    size_t got = 0;
    bool reset = false;
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                int w = wait_fd(fd, POLLIN, deadline);
                if (w == 0) { close(fd); return RES_TIMEOUT; }
                if (w < 0) { reset = true; break; }
                continue;
            }
            reset = true; // e.g. ECONNRESET after a fast 503
            break;
        }
        if (got < sizeof(status_line) - 1) {
            size_t take = sizeof(status_line) - 1 - got;
            if ((size_t)n < take) take = (size_t)n;
            memcpy(status_line + got, buf, take);
            got += take;
            status_line[got] = '\0';
        }
    }
    close(fd);

    int status = 0;
    if (sscanf(status_line, "HTTP/1.%*d %d", &status) != 1) return RES_CONN;
    if (status >= 200 && status < 300) return reset ? RES_CONN : RES_OK;
    if (status == 503) return RES_503;
    return RES_OTHER;
}

static void *client_fn(void *arg) {
    stats_t *s = arg;
    // Distinct, reproducible stream per client: seed and client index.
    unsigned int seed = opt.seed * 7919u + (unsigned int)(s - g_stats) * 104729u + 1u;

    while (!atomic_load(&stop_flag)) {
        int pick = (int)(rand_r(&seed) % 100);
        int route = 0;
        for (int acc = 0; route < 4; route++) {
            acc += opt.mix->weight[route];
            if (pick < acc) break;
        }
        if (route > 3) route = 3;

        double t0 = now_s();
        result_t r = one_request(route, &seed);
        double t1 = now_s();
        // Attribute by start time: a request sent during the window counts
        // even if it ends later, so a frozen server shows up as timeouts.
        bool measuring = t0 >= opt.warmup_s && t0 < opt.warmup_s + opt.duration_s;

        if (measuring) {
            switch (r) {
                // Latency percentiles are over successful requests only;
                // failures are reported as counts.
                case RES_OK: s->ok++; s->per_route_ok[route]++; record(s, (t1 - t0) * 1000.0); break;
                case RES_503: s->s503++; break;
                case RES_OTHER: s->other++; break;
                case RES_TIMEOUT: s->timeouts++; break;
                case RES_CONN: s->conn_err++; break;
            }
        }
        if (r == RES_CONN) {
            struct timespec ts = { 0, 10 * 1000000L };
            nanosleep(&ts, NULL); // Do not spin when the server refuses connections
        }
    }
    return NULL;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(const double *v, size_t n, double p) {
    if (n == 0) return NAN;
    return v[(size_t)(p * (double)(n - 1) + 0.5)];
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--host H] [--port P] [--clients N] [--duration S] [--warmup S]\n"
            "          [--timeout-ms MS] [--mix static|balanced|write] [--seed N]\n"
            "          [--label TEXT] [--csv FILE]\n", prog);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i += 2) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v && strcmp(a, "--help") != 0) { usage(argv[0]); return 2; }
        if (strcmp(a, "--host") == 0) opt.host = v;
        else if (strcmp(a, "--port") == 0) opt.port = atoi(v);
        else if (strcmp(a, "--clients") == 0) opt.clients = atoi(v);
        else if (strcmp(a, "--duration") == 0) opt.duration_s = atof(v);
        else if (strcmp(a, "--warmup") == 0) opt.warmup_s = atof(v);
        else if (strcmp(a, "--timeout-ms") == 0) opt.timeout_ms = atoi(v);
        else if (strcmp(a, "--seed") == 0) opt.seed = (unsigned int)atoi(v);
        else if (strcmp(a, "--label") == 0) opt.label = v;
        else if (strcmp(a, "--csv") == 0) opt.csv = v;
        else if (strcmp(a, "--mix") == 0) {
            opt.mix = NULL;
            for (size_t k = 0; k < sizeof(mixes) / sizeof(mixes[0]); k++) {
                if (strcmp(v, mixes[k].name) == 0) opt.mix = &mixes[k];
            }
            if (!opt.mix) { fprintf(stderr, "Unknown mix '%s'\n", v); return 2; }
        } else { usage(argv[0]); return 2; }
    }
    if (opt.clients <= 0 || opt.duration_s <= 0) { usage(argv[0]); return 2; }
    signal(SIGPIPE, SIG_IGN);

    stats_t *stats = calloc((size_t)opt.clients, sizeof(stats_t));
    pthread_t *tids = calloc((size_t)opt.clients, sizeof(pthread_t));
    if (!stats || !tids) { perror("calloc"); return 1; }
    g_stats = stats;

    clock_gettime(CLOCK_MONOTONIC, &t_begin);
    atomic_store(&stop_flag, false);
    for (int i = 0; i < opt.clients; i++) {
        if (pthread_create(&tids[i], NULL, client_fn, &stats[i]) != 0) {
            fprintf(stderr, "Could not start client %d\n", i);
            return 1;
        }
    }

    struct timespec ts = { (time_t)(opt.warmup_s + opt.duration_s), 0 };
    ts.tv_nsec = (long)((opt.warmup_s + opt.duration_s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    atomic_store(&stop_flag, true);
    for (int i = 0; i < opt.clients; i++) pthread_join(tids[i], NULL);

    stats_t tot = {0};
    for (int i = 0; i < opt.clients; i++) {
        tot.ok += stats[i].ok;
        tot.s503 += stats[i].s503;
        tot.other += stats[i].other;
        tot.timeouts += stats[i].timeouts;
        tot.conn_err += stats[i].conn_err;
        for (int r = 0; r < 4; r++) tot.per_route_ok[r] += stats[i].per_route_ok[r];
        for (size_t k = 0; k < stats[i].n; k++) record(&tot, stats[i].lat[k]);
        free(stats[i].lat);
    }
    qsort(tot.lat, tot.n, sizeof(double), cmp_double);

    uint64_t total = tot.ok + tot.s503 + tot.other + tot.timeouts + tot.conn_err;
    double rps = (double)total / opt.duration_s;
    double goodput = (double)tot.ok / opt.duration_s;
    double p50 = pct(tot.lat, tot.n, 0.50), p95 = pct(tot.lat, tot.n, 0.95);
    double p99 = pct(tot.lat, tot.n, 0.99), pmax = tot.n ? tot.lat[tot.n - 1] : NAN;

    printf("%-10s mix=%-8s clients=%-4d | ok=%-6llu 503=%-5llu other=%-4llu timeout=%-4llu connerr=%-4llu"
           " | goodput=%8.1f/s | p50=%7.1f p95=%7.1f p99=%7.1f ms\n",
           opt.label, opt.mix->name, opt.clients,
           (unsigned long long)tot.ok, (unsigned long long)tot.s503,
           (unsigned long long)tot.other, (unsigned long long)tot.timeouts,
           (unsigned long long)tot.conn_err, goodput, p50, p95, p99);

    if (opt.csv) {
        FILE *probe = fopen(opt.csv, "r");
        bool need_header = probe == NULL;
        if (probe) fclose(probe);
        FILE *f = fopen(opt.csv, "a");
        if (!f) { perror(opt.csv); return 1; }
        if (need_header) {
            fprintf(f, "label,mix,clients,seed,duration_s,requests,ok,rejected_503,other_errors,"
                       "timeouts,conn_errors,throughput_rps,goodput_rps,p50_ms,p95_ms,p99_ms,max_ms,"
                       "ok_static,ok_query,ok_upload,ok_refresh\n");
        }
        fprintf(f, "%s,%s,%d,%u,%.1f,%llu,%llu,%llu,%llu,%llu,%llu,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,"
                   "%llu,%llu,%llu,%llu\n",
                opt.label, opt.mix->name, opt.clients, opt.seed, opt.duration_s,
                (unsigned long long)total, (unsigned long long)tot.ok,
                (unsigned long long)tot.s503, (unsigned long long)tot.other,
                (unsigned long long)tot.timeouts, (unsigned long long)tot.conn_err,
                rps, goodput, p50, p95, p99, pmax,
                (unsigned long long)tot.per_route_ok[0], (unsigned long long)tot.per_route_ok[1],
                (unsigned long long)tot.per_route_ok[2], (unsigned long long)tot.per_route_ok[3]);
        fclose(f);
    }

    free(tot.lat);
    free(stats);
    free(tids);
    return 0;
}
