# SafeServe

**SafeServe** is a multi-threaded HTTP/1.1 web server written in C11 (POSIX threads) featuring **deadlock-safe admission control** powered by **Dijkstra's Banker's Algorithm**.

Built as an Operating Systems comparative study, SafeServe demonstrates and benchmarks four concurrency control strategies:
1. **Banker's Avoidance (`mode = banker`)** *(SafeServe default)*: Evaluates global system state safety before granting resource claims; guarantees zero deadlocks.
2. **Naive Baseline (`mode = naive`)**: Uncoordinated per-resource acquisition; deterministically hits circular-wait deadlock under conflicting concurrent workloads.
3. **Lock Ordering Prevention (`mode = lockorder`)**: Strict hierarchical resource indexing that mathematically prevents circular wait.
4. **Detection and Recovery (`mode = detect`)**: Background matrix-based detection thread that periodically detects deadlocks and aborts victim requests.

---

## Architecture Overview

```
Client ──> Acceptor Thread ──> Bounded MPMC Queue ──> Worker Pool
                                                            │
                                              Parse Request ──> Route Claim Lookup
                                                            │
                                             Resource Manager (Banker's)
                                              ├── Safe?   ──> Execute Handler ──> Release & Broadcast
                                              └── Unsafe? ──> Wait on CondVar (Timeout ──> 503)
```

### Shared Resource Pool
- **`WORKER`**: Logical worker thread execution slots (default: 16).
- **`BUF`**: Buffer memory units (default: 64).
- **`FD`**: File descriptor allocation budget (default: 128).
- **`DB`**: Simulated database connection pool (default: 4).
- **`CLOCK`**: Cache write locks (default: 2).

### Route Claims
| Route | Method | Resources Claimed | Behavior |
|---|:---:|:---:|---|
| `/health` | GET | None (0 claim) | Immediate 200 OK liveness check |
| `/metrics` | GET | None (0 claim) | Real-time JSON latency percentiles & counters |
| `/static/*` | GET | WORKER: 1, BUF: 2, FD: 2 | Serves files from `docroot` (`www/`) |
| `/query` | GET | WORKER: 1, BUF: 8, FD: 1, DB: 1 | Simulated database query |
| `/upload` | POST | WORKER: 1, BUF: 16, FD: 4, CLOCK: 1 | Buffered upload with cache write lock |
| `/refresh` | POST | WORKER: 1, BUF: 4, FD: 2, DB: 1, CLOCK: 1 | DB read + cache rewrite (competing claim) |

---

## Quickstart

### 1. Build
```bash
make            # Compiles bin/safeserve
make test       # Runs unit and concurrent stress tests
make demo       # Runs automated Deadlock vs. Banker's avoidance demo
```

### 2. Sanitizers (Verification)
```bash
make asan       # Clean build & test under AddressSanitizer (memory/buffer safety)
make tsan       # Clean build & test under ThreadSanitizer (data-race detection)
```

### 3. Run the Server
```bash
./bin/safeserve --port 8080 --mode banker
```

### 4. Deadlock Demo
To see the naive baseline freeze under circular wait and SafeServe succeed under identical conditions:
```bash
make demo
```
Or run the live network demonstration script:
```bash
./tools/deadlock_demo.sh
```

---

## Evaluation & Benchmarks
To benchmark with `wrk`:
```bash
# Start server in background
./bin/safeserve --port 8080 --mode banker &

# Run 100 concurrent connections across 4 threads
wrk -t4 -c100 -d10s http://127.0.0.1:8080/query

# Inspect live metrics
curl http://127.0.0.1:8080/metrics
```

To plot comparative benchmark charts:
```bash
python3 tools/plot.py
```
Outputs high-resolution charts to `tools/benchmark_results.png`.
