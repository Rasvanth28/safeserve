# SafeServe

SafeServe is a multi-threaded HTTP/1.1 server in C11 with POSIX threads. Every request needs several limited resources at once (worker slots, buffer memory, file descriptors, DB connections, cache locks). An **admission controller** decides when a request may take each one, and the server can run under four deadlock strategies so they can be compared on the same workload:

| Mode | Strategy | What happens on the circular-wait workload |
|---|---|---|
| `naive` | None | Requests take resources in conflicting orders and **deadlock**; they stay blocked forever. |
| `lockorder` | Prevention | Every handler takes resources in one global order (`WORKER < BUF < FD < DB < CLOCK`), so a cycle cannot form. |
| `detect` | Detection and recovery | Deadlocks happen; a detector thread finds them (Allocation/Request matrices) and aborts a victim with `503`. |
| `banker` | Avoidance (default) | Dijkstra's Banker's algorithm grants a request only if the state stays **safe**; otherwise it waits (bounded, then `503`). |

Built as an Operating Systems course project. Design: [`docs/EDD.md`](docs/EDD.md). Requirements: [`docs/PRD.md`](docs/PRD.md). Results and discussion: [`docs/REPORT.md`](docs/REPORT.md).

## Quickstart

```bash
make                # bin/safeserve and bin/loadgen (no dependencies beyond libc + pthreads)
make test           # unit, property and concurrency tests
make e2e            # starts the server and tests it over HTTP
make demo           # in-process demo: one workload under all four strategies
make demo-live      # the same demo against the real HTTP server
```

```bash
./bin/safeserve --mode banker --port 8080
curl localhost:8080/health
curl localhost:8080/metrics
open http://localhost:8080/          # live dashboard
```

Linux and macOS are supported. CI runs everything on Ubuntu, including the sanitizer builds.

## How it works

```
client ─▶ acceptor ─▶ bounded queue ─▶ worker pool (16)
           │ full? 503   │ waited > queue_timeout? 503
                                       │
                         parse ─▶ route ─▶ register claim (Max vector)
                                       │
                         handler acquires resources step by step
                         ┌─────────── resource manager ───────────┐
                         │ naive:     grant if free, else wait      │
                         │ lockorder: grant if free and in order    │
                         │ detect:    grant if free; detector       │
                         │            thread aborts victims         │
                         │ banker:    grant only if state is safe   │
                         └──────────────────────────────────────────┘
                                       │
                         run handler ─▶ release all ─▶ broadcast
```

* **Resource manager** (`src/rm.c`): one mutex and condition variable over the `Available`, `Max`, `Allocation` and `Request` tables. `Need` is always derived as `Max − Allocation`.
* **Admission modes** (`admit_mode`):
  * `incremental` (default): handlers acquire one resource type at a time and hold what they have while waiting for the next. This hold-and-wait is what makes deadlock possible, and it's the case Banker's exists for.
  * `atomic`: the whole claim is reserved before the handler runs. That alone removes hold-and-wait, so no mode can deadlock. Useful as a comparison point.
* **Conflicting routes**: `/upload` takes `CLOCK` then `DB`; `/refresh` takes `DB` then `CLOCK`.
* **Fairness** (banker): FIFO among waiters, plus aging. Once a request has waited `fairness_aging_ms`, new requests stand aside so it isn't starved.
* **Overload**: when the queue is full the server answers `503` immediately. A request that sat in the queue longer than `queue_timeout_ms` gets `503` without being run. A request that can't be admitted within `admit_timeout_ms` gets `503` with `Retry-After`.

### Resources and route claims (`config/resources.conf`)

| Resource | Total | | Route | Method | WORKER | BUF | FD | DB | CLOCK |
|---|---:|---|---|---|---:|---:|---:|---:|---:|
| `WORKER` | 16 | | `/static/*`, `/` | GET | 1 | 2 | 2 | 0 | 0 |
| `BUF` | 64 | | `/query` | GET | 1 | 8 | 1 | 1 | 0 |
| `FD` | 128 | | `/upload` | POST | 1 | 16 | 4 | 1 | 1 |
| `DB` | 4 | | `/refresh` | POST | 1 | 4 | 2 | 1 | 1 |
| `CLOCK` | 2 | | `/health`, `/metrics` | GET | 0 | 0 | 0 | 0 | 0 |

Zero-claim routes bypass the resource manager, so `/health` and `/metrics` keep answering even when the server is deadlocked.

## Command line

```
./bin/safeserve [--config config/server.conf] [--resources config/resources.conf]
                [--mode banker|naive|lockorder|detect] [--admit-mode incremental|atomic]
                [--port N] [--workers N] [--docroot DIR] [--detect-interval MS]
                [--util-csv FILE] [--quiet]
```

Every option in `config/server.conf` is commented. `SIGINT`/`SIGTERM` trigger a graceful shutdown:
1. Stop accepting connections.
2. Let queued and in-flight requests finish (3 s grace).
3. Abort anything still blocked, e.g. a naive deadlock.
4. Join all threads.

## Testing

| Suite | What it checks |
|---|---|
| `test_banker` | Silberschatz textbook cases, boundaries, and a property test comparing the safety and request algorithms with brute force over every ordering (20,000 random states). |
| `test_detect` | Textbook detection example, running/inactive slots, victim choice, lock-order rule. |
| `test_modes` | The circular-wait workload under each strategy: naive deadlocks, banker/lockorder complete, detect aborts exactly one victim. Also checks claim enforcement, and that a large claim isn't starved by a stream of small ones. |
| `test_rm_stress` | 24 threads × 60 requests with random acquisition orders per mode. A checker thread asserts the EDD §11.4 invariants (and safety, in banker mode) every millisecond. |
| `test_http`, `test_queue` | Parser (malformed input, percent-decoding, 413/501), traversal and symlink escapes, MPMC queue. |
| `tests/e2e.sh` | Real server over HTTP: status codes, 405 + `Allow`, split and 200 KB requests, `/metrics` JSON, graceful shutdown, conflicting load in every mode. |

```bash
make asan    # unit + e2e under AddressSanitizer
make tsan    # unit tests under ThreadSanitizer
```

## Benchmarks

```bash
make bench                         # sweep -> results/bench.csv (+ results/bench_meta.txt)
REPS=5 DURATION=60 make bench      # the full EDD §12 protocol (slow)
make plot                          # results/bench.png (needs matplotlib)
```

`tools/bench.sh` starts a fresh server for every combination of strategy, workload mix (`static`, `balanced`, `write`) and concurrency level, then drives it with `bin/loadgen`. `loadgen` uses closed-loop clients with fixed seeds, warm-up, and client-side timeouts, so a frozen server shows up as timeouts. Every plotted number comes from the CSV. The committed results and their discussion are in [`docs/REPORT.md`](docs/REPORT.md).

## Layout

```
src/        server: main, net, http, queue, worker, handlers, rm (+ banker, naive,
            lockorder, detect policies), metrics, config, log
include/    headers
tests/      C test suites and e2e.sh
tools/      loadgen.c, bench.sh, plot.py, demo_runner.c, deadlock_demo.sh
config/     server.conf, resources.conf
www/        static files and the live dashboard
docs/       PRD, EDD, report
results/    benchmark CSV, metadata and plot
```

## Limitations

* Every response is sent with `Connection: close`; there is no keep-alive (PRD FR-6 is "Could").
* The database and the upload work are simulated with configurable delays. Resource contention is real; the I/O isn't.
* Under heavy overload, queue-full rejections close the socket before reading the request, so clients may see a connection reset instead of the `503` body.
* Banker's needs declared maximum claims. A handler that asks for more than its claim gets a `500`.
