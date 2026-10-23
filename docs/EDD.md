# SafeServe: Engineering Design Document (EDD)

**Project:** SafeServe, a multi-threaded web server with deadlock-safe admission control
**Language:** C11, POSIX threads, Linux
**Status:** v1.1 (implemented; see §18 for as-built changes)
**Last updated:** 2026-10-23
**Companion:** `PRD.md`, `REPORT.md`

---

## 1. Purpose

This document describes how SafeServe will be built to satisfy the requirements in the PRD. It covers architecture, data structures, algorithms, concurrency, configuration, testing, and evaluation.

## 2. System Overview

```
Client → Acceptor thread → Bounded request queue → Worker thread
                                                     │
                                    Parse request → Route lookup (Max claim)
                                                     │
                                         Resource Manager (Banker's)
                                          │ safe?                │ unsafe
                                          ▼                      ▼
                                       Handler              wait on condvar
                                          │                 (timeout → 503)
                                          ▼
                               Release resources → broadcast → send response
```

**Threads**
- 1 acceptor thread
- N worker threads (default 16)
- 1 deadlock-detector thread (detection mode only)
- 1 optional metrics/flush thread

Workers do parsing, admission, handling, and response, so a worker blocked in admission occupies a pool slot. The bounded queue and admission timeout keep this bounded.

## 3. Module Breakdown

```
safeserve/
├── Makefile
├── README.md
├── config/
│   ├── server.conf          # port, threads, mode, timeouts
│   └── resources.conf       # resource pools and route claims
├── include/
│   ├── net.h  http.h  queue.h  rm.h  banker.h
│   ├── lockorder.h  detect.h  handlers.h  metrics.h  config.h  log.h
├── src/
│   ├── main.c               # startup, signals, shutdown
│   ├── net.c                # socket/bind/listen/accept
│   ├── http.c               # parser, response writer
│   ├── queue.c              # bounded MPMC queue
│   ├── worker.c             # worker loop
│   ├── rm.c                 # resource manager facade (mode dispatch)
│   ├── banker.c             # safety algorithm + request algorithm
│   ├── lockorder.c          # prevention baseline
│   ├── naive.c              # no-protection baseline
│   ├── detect.c             # wait-for graph + victim selection
│   ├── handlers.c           # static, upload, query, refresh, metrics, health
│   ├── metrics.c            # counters, latency histogram
│   ├── config.c             # config parser
│   └── log.c
├── tests/
│   ├── test_banker.c        # textbook cases, randomized checks
│   ├── test_queue.c
│   ├── test_http.c
│   └── test_rm_stress.c
├── tools/
│   ├── loadgen.c (or loadgen.py)
│   ├── deadlock_demo.sh
│   └── plot.py
└── docs/ (PRD.md, EDD.md, report)
```

## 4. Resource Model

### 4.1 Resource types

| Index | Name | Instances (default) | Notes |
|---|---|---|---|
| 0 | `WORKER` | 16 | Logical worker slots |
| 1 | `BUF` | 64 | Buffer memory units |
| 2 | `FD` | 128 | Reserved file-descriptor budget |
| 3 | `DB` | 4 | Database connections |
| 4 | `CLOCK` | 2 | Cache write locks |

Counts and names come from `resources.conf`. The maximum number of resource types `M_MAX` is 8 and the maximum number of concurrent requests (processes) `N_MAX` is 1024, both compile-time constants.

### 4.2 Route claims (default)

| Route | WORKER | BUF | FD | DB | CLOCK |
|---|---|---|---|---|---|
| `/static/*` | 1 | 2 | 2 | 0 | 0 |
| `/upload` | 1 | 16 | 4 | 0 | 1 |
| `/query` | 1 | 8 | 1 | 1 | 0 |
| `/refresh` | 1 | 4 | 2 | 1 | 1 |
| `/metrics`, `/health` | 0 | 0 | 0 | 0 | 0 |

A claim is the Banker's `Max` vector for that request.

### 4.3 Configuration format

Simple line-based `key = value` with sections, to avoid external parsers.

```ini
[resources]
WORKER = 16
BUF    = 64
FD     = 128
DB     = 4
CLOCK  = 2

[route /upload]
WORKER = 1
BUF    = 16
FD     = 4
CLOCK  = 1
```

```ini
[server]
port           = 8080
workers        = 16
queue_capacity = 256
mode           = banker       # naive | lockorder | detect | banker
admit_mode     = incremental  # atomic | incremental
admit_timeout_ms = 2000
detect_interval_ms = 200
docroot        = ./www
```

## 5. Core Data Structures

```c
#define M_MAX 8
#define N_MAX 1024

typedef struct {
    int m;                         // number of resource types
    int total[M_MAX];              // total instances
    int available[M_MAX];          // currently free
    struct req_slot {
        int  active;               // slot in use
        int  max[M_MAX];           // declared claim
        int  alloc[M_MAX];         // currently held
        uint64_t ticket;           // FIFO ticket for fairness
    } slot[N_MAX];
    pthread_mutex_t lock;          // global lock over all tables
    pthread_cond_t  cv;            // signalled on every release
    uint64_t next_ticket;
    uint64_t serving_ticket;       // for FIFO ordering (optional)
} rm_t;
```

`need[i][j]` is computed as `max - alloc` instead of stored, which removes a class of consistency bugs.

```c
typedef struct {
    int fd;                        // client socket
    struct timespec t_accept;
} job_t;

typedef struct {                   // bounded queue
    job_t *buf; int cap, head, tail, count;
    pthread_mutex_t mu;
    pthread_cond_t not_empty, not_full;
} queue_t;
```

## 6. Algorithms

### 6.1 Safety algorithm (Banker's)

Input: `available`, `alloc[]`, `max[]` for active slots.

```
work   = copy(available)
finish = [false] for each active slot
loop:
    find active slot i with finish[i] == false and (max[i] - alloc[i]) <= work
    if none: break
    work += alloc[i]; finish[i] = true
return all finish[i] == true     # safe
```

Complexity is O(n² · m) per check, with n active requests and m resource types. With n ≤ 64 and m ≤ 8 this stays well under the 100 µs target (NFR-3), and the benchmark will measure it.

### 6.2 Resource-request algorithm

`rm_acquire(slot, req[])` is called with the global lock held:

```
1. if any req[j] > max[slot][j] - alloc[slot][j]:   error: exceeds declared claim
2. if any req[j] > available[j]:                    must wait (not enough free)
3. tentatively: available -= req; alloc[slot] += req
4. if safe_state():                                 commit, return GRANTED
   else: rollback (available += req; alloc[slot] -= req); must wait
```

On "must wait": `pthread_cond_timedwait(&cv, &lock, deadline)`; loop and retry on wakeup. On timeout return `E_TIMEOUT` (→ HTTP 503). Every `rm_release` ends with `pthread_cond_broadcast(&cv)`.

### 6.3 Admission modes

**Atomic reservation (`admit_mode = atomic`)**
The worker requests its entire claim in one `rm_acquire` call. The request either gets everything or waits. Simple, safe, and good for the first working version.

**Incremental (`admit_mode = incremental`)**
At admission the request registers `max` with `alloc = 0`. A registered request with zero allocation is always safe as long as `max ≤ total` (it can run alone once others finish), so the real gating happens at each `rm_acquire` step. Handlers acquire resources in the order their logic needs them, for example `FD` → `BUF` → `DB`. This is the faithful Banker's model and the version used to show that avoidance handles arbitrary acquisition order.

### 6.4 Fairness

A pure "retry when anything is released" policy can starve large claims. Mitigation:
- Each waiter takes a **ticket** on entering the wait loop.
- On wakeup, a waiter may proceed only if no older waiter's request could currently be granted. This is checked by running the same safe-grant test on the oldest waiters first.
- Optional aging: after `aging_ms` a waiter blocks younger requests that would consume resources it needs.

Start with plain FIFO tickets. Add aging only if the benchmark shows starvation.

### 6.5 Baselines

**Naive (`mode = naive`)**
`acquire` takes a per-resource mutex or counting semaphore, one resource at a time, with no checks. Under the demo workload (route A takes DB then CLOCK, route B takes CLOCK then DB, with a `usleep` between acquisitions) it deadlocks deterministically.

**Lock ordering (`mode = lockorder`)**
Resources have fixed global indices. Handlers must acquire in increasing index order. `rm_acquire` asserts the order in debug builds and sorts the claim in release builds. Deadlock is impossible because a cycle would require an order violation.

**Detection and recovery (`mode = detect`)**
- Each blocked request records what it waits for. A wait-for graph is built every `detect_interval_ms` by the detector thread.
- Resources are multi-instance, so detection uses the matrix-based algorithm (Allocation, Request, Available) and not simple cycle detection on a single-instance graph.
- Victim selection: the request with the least allocated resources, with a tiebreak on youngest ticket.
- Recovery: mark the victim aborted, release its resources, wake it, and return `503`/`500`. The victim's handler must check the abort flag after each wait and clean up.

## 7. Concurrency Design

| Shared state | Protection | Notes |
|---|---|---|
| Resource tables (`rm_t`) | One global mutex + condvar | Correct first; optimize only if profiling shows contention |
| Request queue | Queue mutex + two condvars | Acceptor blocks or rejects when full per FR-8 |
| Metrics counters | Atomic increments (`stdatomic.h`) | Latency histogram with a small mutex or per-thread buckets merged on read |
| Log output | Single mutex around `write` or one `fprintf` per line | Keeps lines unbroken |
| Shutdown flag | `atomic_bool` | Checked by acceptor and workers |

**Lock hierarchy:** the `rm` lock is never held while doing I/O or acquiring any other lock. Handlers call into `rm` only to acquire or release.

**Shutdown sequence**
1. Signal handler sets `shutdown`.
2. Acceptor closes the listening socket and exits.
3. Queue is marked closed and workers drain remaining jobs.
4. Waiters in `rm` are woken and return `E_SHUTDOWN`.
5. `pthread_join` all threads, free memory, close log.

## 8. HTTP Layer

- Read with a per-connection buffer (8 KB headers max, configurable body limit); use `recv` with a timeout (`SO_RCVTIMEO`) to avoid slowloris stalls.
- Parse the request line (method, path, version), headers (case-insensitive), and body by `Content-Length`.
- Responses are built with status line, `Content-Type`, `Content-Length`, `Connection`, and `Retry-After` on 503.
- Path sanitization: reject any decoded path containing `..` or a NUL byte; resolve with `realpath` and confirm it stays inside `docroot`.
- Status codes used: 200, 400, 404, 405, 413, 500, 503.

## 9. Handlers

| Route | Behavior | Resources |
|---|---|---|
| `GET /static/*` | `open`, `fstat`, loop `read`/`send` using a buffer from the BUF pool | WORKER, BUF, FD |
| `POST /upload` | Read body into buffer, write to a temp file, then rename under a cache lock | WORKER, BUF, FD, CLOCK |
| `GET /query?...` | Simulated or SQLite query with a configurable artificial delay | WORKER, BUF, FD, DB |
| `POST /refresh` | Read from DB, rewrite a cache entry under lock | WORKER, BUF, FD, DB, CLOCK |
| `GET /metrics` | Dump counters, latency percentiles, current allocation table | none |
| `GET /health` | Return `200 ok` | none |

Artificial delays (`usleep`) are configurable per route so that resource holding times are visible in experiments.

## 10. Metrics and Logging

**Per-request log line:** `ts route status total_ms queue_ms admit_ms bytes`.

**Counters:** requests served, 2xx/4xx/5xx counts, rejected (queue full), timed out (admission), safety checks run, safety-check total time, deadlocks detected and victims aborted (detect mode).

**Latency:** fixed-bucket histogram (log-spaced) to derive p50, p95, p99 without storing every sample. The load generator also records exact client-side latencies.

**Utilization:** a sampler records `total - available` per resource at 100 ms intervals into a CSV.

## 11. Testing Strategy

### 11.1 Unit tests
- **Banker's:** classic textbook state (5 processes, 3 resource types) with a known safe sequence; a known unsafe state; a request that must be denied; boundary cases (zero need, need equals total, claim exceeds total).
- **Property test:** generate random states, compare the safety result to a brute-force search over all permutations (n ≤ 7).
- **Queue:** concurrent producers/consumers, full/empty behavior, close semantics.
- **HTTP parser:** malformed request lines, oversized headers, missing `Content-Length`, path traversal.

### 11.2 Integration and stress
- `test_rm_stress.c`: spawn many threads that acquire random subsets within their claims with random delays; assert that all complete, that invariants hold (`available + Σ alloc == total`), and that no thread hangs past a watchdog.
- End-to-end: `loadgen` against each mode for at least 10 minutes at 1000 clients in banker mode (NFR-2).

### 11.3 Tooling
- ThreadSanitizer (`-fsanitize=thread`) and Helgrind for races.
- AddressSanitizer and Valgrind for memory errors and leaks.
- `gdb -p` / `pstack` thread dumps to show the naive deadlock in the demo.

### 11.4 Invariants (asserted in debug builds)
1. `0 ≤ available[j] ≤ total[j]`.
2. `available[j] + Σ_i alloc[i][j] == total[j]`.
3. `alloc[i][j] ≤ max[i][j]`.
4. The committed state after every grant is safe.

## 12. Evaluation Plan

**Variables**
- Mode: naive, lockorder, detect, banker (atomic and incremental).
- Concurrency: 10, 100, 500, 1000 clients.
- Workload mix: static-heavy (90% static), balanced, upload/DB-heavy.
- Resource pool size: default and halved (to induce contention).

**Metrics:** throughput, p50/p95/p99 latency, rejection and timeout rate, deadlocks (count and duration frozen), resource utilization, safety-check overhead.

**Method:** warm-up of 10 s, measurement of 60 s, at least 5 repetitions, report median and spread. Use `wrk` or the custom `loadgen` for HTTP load; the custom generator is needed to drive mixed routes and POST bodies.

**Expected outcome:** naive freezes under the crafted mix; lockorder and banker complete with no deadlock; banker shows a small throughput cost and possibly lower utilization from conservative admission; detect shows wasted work and latency spikes from aborted victims.

## 13. Build and Run

```bash
make                 # builds ./safeserve and tests
make test            # unit tests
make tsan            # ThreadSanitizer build
./safeserve --config config/server.conf --mode banker
./tools/deadlock_demo.sh      # naive vs. safeserve under the same workload
./tools/loadgen --url http://localhost:8080 --clients 500 --mix balanced --duration 60
```

Compiler flags: `-std=c11 -Wall -Wextra -Werror -O2 -g -pthread`.

## 14. Implementation Order

1. `net.c`, `http.c`, single-threaded static server.
2. `queue.c`, `worker.c`, thread pool, logging.
3. `banker.c` with unit tests (no server dependency).
4. `rm.c` with atomic mode, then handlers declaring claims.
5. `naive.c` plus `deadlock_demo.sh` to reproduce the freeze.
6. Incremental mode, timeouts, 503 path, fairness tickets.
7. `lockorder.c` and `detect.c`.
8. Metrics, `/metrics`, CSV output, `loadgen`, plots.
9. Stress runs, sanitizer passes, report.

## 15. Design Trade-offs and Decisions

| Decision | Choice | Reason | Alternative |
|---|---|---|---|
| Resource manager locking | One global mutex | Simple and provably correct; contention is low at expected scale | Per-resource locks (risk of manager-level deadlock) |
| Claim source | Static per-route config | Banker's needs max claims in advance | Learned profiles (stretch) |
| Thread model | Fixed pool, blocking I/O | Matches OS course topics | `epoll` event loop (stretch comparison) |
| Waiting | Condvar with timeout | Bounded latency, clean 503 | Spin or poll (wasteful) |
| `need` storage | Derived from `max - alloc` | Avoids inconsistency | Stored matrix |
| Overload handling | Queue limit + admission timeout | Predictable tail latency | Unbounded queue (memory risk) |

## 16. Risks and Technical Mitigations

| Risk | Mitigation |
|---|---|
| Worker threads blocked in admission starve cheap requests | Admit zero-claim routes (`/health`, `/metrics`) before worker acquisition; size queue and timeout; report effect |
| Lost wakeups on the condvar | Always wait in a predicate loop; broadcast on every release |
| Handler exceeds its declared claim | `rm_acquire` returns an error; handler returns 500; log as a bug |
| Handler crashes while holding resources | Use cleanup via a single exit path (`goto cleanup`) that calls `rm_release_all(slot)` |
| Timing-dependent baseline demo is flaky | Deterministic order with fixed `usleep` and a barrier so both requests hold their first resource before requesting the second |
| Benchmark noise from the client sharing the machine | Pin server and load generator to different cores; repeat runs |

## 17. Open Questions (resolved)

1. Default admission mode: **incremental**. Atomic reservation removes hold-and-wait by itself, so under atomic no strategy can deadlock and the comparison is empty. Atomic stays available as `banker:atomic` in the benchmark.
2. DB: **simulated** with a configurable delay. Contention on the DB pool is real; the query is not.
3. Keep-alive: **out of scope**. Every response is `Connection: close`.
4. Plots: **Python/matplotlib** (`tools/plot.py`), reading only the benchmark CSV.

## 18. As-Built Notes

Where the implementation differs from or refines the design above, and why.

| Area | Design | As built | Reason |
|---|---|---|---|
| Strategy modules | Per-strategy lock sets (per-resource mutexes for naive and lockorder) | One set of tables in `rm.c` behind one mutex; `naive.c`, `lockorder.c`, `detect.c` and `banker.c` are pure policy functions over those tables | All four strategies see identical accounting, and the policies are unit-testable without threads. Deadlock comes from hold-and-wait plus circular wait, not from the lock granularity, so naive still deadlocks. |
| Waiting | All modes time out after `admit_timeout_ms` | Banker and lockorder time out. Naive and detect wait until granted, aborted or shut down | If naive waits time out, a deadlock turns into slow 503s instead of a freeze. In detect mode only the detector should break deadlocks. |
| Admission timeout | Per acquisition call | One deadline per request, shared by all incremental steps | Keeps admission latency bounded by `admit_timeout_ms` (G3) instead of steps × timeout. |
| Detection input | Wait-for graph / `Max − Alloc` | Request matrix: `request[i]` is set only while slot *i* is blocked | `Max − Alloc` is Banker's safety test, not detection. With the Request matrix, a running request is never mistaken for a deadlocked one. |
| Victim recovery | Mark aborted, release, wake | Same; victims are always blocked in `rm_acquire_vec` when chosen, so their resources are never revoked while in use | Correctness of revocation. |
| Lock ordering | Sort claim in release builds, assert in debug | Handlers' plans are sorted by `lockorder_sort_plan`; any downhill request at runtime returns `RM_ORDER` (500) and is counted | Enforced in every build, observable in `/metrics`. |
| Fairness (§6.4) | FIFO tickets; aging only if needed | FIFO rule plus aging (`fairness_aging_ms`, default 250 ms). After that wait, slots holding nothing yield to the aged waiter; the FIFO rule only defers to waiters the aging rule is not holding back | The FIFO rule alone did not prevent starvation of a 4-unit claim among 1-unit claims (`test_modes`). A first version without the "not held back" condition livelocked under load: two waiters each yielded to the other. |
| Overload | Queue limit + admission timeout | Also `queue_timeout_ms` (default 1 s): a request that waited longer in the queue gets 503 without running | Without it, at 200 clients workers spent all their time serving requests whose clients had already timed out, and goodput fell to zero in every mode. |
| `/upload` claim | WORKER, BUF, FD, CLOCK | Adds `DB: 1` (upload records itself in the DB after taking the cache lock) | Gives the opposite-order pair `/upload` (CLOCK→DB) and `/refresh` (DB→CLOCK). |
| Zero-claim routes | Admit before worker acquisition | `/health` and `/metrics` skip the resource manager entirely | They stay available during a naive deadlock, which the demo relies on. |
| Load generator | `loadgen` (C or Python) | `tools/loadgen.c`: closed-loop, seeded, attributes requests by start time | A frozen server shows up as timeouts, not as an empty measurement window. |

