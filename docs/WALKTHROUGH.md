# SafeServe Walkthrough

A guided tour of the code. Read it top to bottom with the repo open. Each stop names the file, what to look at, and the OS concept it shows.

> Video: [`docs/demo/safeserve-demo.mp4`](demo/safeserve-demo.mp4). Re-record it with `vhs docs/demo/demo.tape`.

---

## 0. Run it first (2 minutes)

```bash
make && make test      # everything builds and passes
make demo              # the whole project in one screen
make demo-live         # the same against the real HTTP server
```

`make demo` runs one workload under the four strategies. Two threads need the same two resources (one DB connection, one cache lock) in opposite orders:

| Mode | What you see |
|---|---|
| naive | both threads grab one resource each, then wait forever → **DEADLOCK** |
| banker | `/upload`'s first request is **refused as unsafe**; it waits, then runs after `/refresh` |
| lockorder | `/upload` reorders its plan to DB first; no cycle can form |
| detect | the deadlock happens; the detector aborts one victim after about 100 ms |

Everything below explains how the code makes that happen.

---

## 1. A request's life: `src/main.c` → `src/worker.c`

**Acceptor thread** (`main.c`, the `while (g_keep_running)` loop):
1. `accept()` a connection.
2. Set a 5 s receive timeout, which stops slowloris clients from pinning a worker.
3. `queue_try_push()` the connection. If the bounded queue is full, answer `503` right away (FR-8).

**Bounded MPMC queue** (`src/queue.c`): a ring buffer protected by a mutex, with `not_empty`/`not_full` condition variables. Workers sleep in `pthread_cond_wait` until a job arrives. This is the classic producer–consumer problem.

**Worker thread** (`worker.c: worker_thread_fn`), for each job:
1. `http_read_request()` loops over `recv()` until the headers and the whole body have arrived.
2. If the request sat in the queue longer than `queue_timeout_ms`, the client has probably given up, so answer `503` without doing the work (overload shedding).
3. `serve_request()`:
   - Find the route; send 404 or 405 when there's no match.
   - Look up the route's **claim**, its maximum resource vector, in `config/resources.conf`.
   - Zero-claim routes (`/health`, `/metrics`) skip the resource manager, so they still answer during a deadlock.
   - Otherwise `rm_alloc_slot()` and `rm_register_claim()`: the request becomes a Banker's "process" with a `Max` row.
   - Run the handler, then `rm_free_slot()` releases everything and wakes waiters.
4. Record the real status code and latency, and log one line.

**Graceful shutdown** (end of `main.c`): `SIGINT`/`SIGTERM` interrupt `accept()`. Workers can't receive the signal because it's blocked with `pthread_sigmask` before they're created. Shutdown then:
1. Closes the queue.
2. Waits up to 3 s for in-flight work.
3. Calls `rm_shutdown()` to wake anything still blocked, such as a naive deadlock.
4. Joins all threads.

---

## 2. Handlers acquire resources step by step: `src/handlers.c`

Look at `acquire_plan()` and the four handlers that use it:

```c
handle_upload:  { "WORKER", "BUF", "FD", "CLOCK", "DB" }   // CLOCK before DB
handle_refresh: { "WORKER", "BUF", "FD", "DB", "CLOCK" }   // DB before CLOCK
```

The handler calls `rm_acquire()` once per resource type and **keeps what it already has while waiting for the next**. That hold-and-wait, plus the opposite orders, is what makes deadlock possible. `step_delay_ms` (50 ms) pauses before the last step, the window in which the other request grabs its first lock.

`admit_mode = atomic` takes the whole claim in one call instead (`worker.c`). That removes hold-and-wait, so no mode can deadlock. It's kept as a comparison point (`banker-atomic` in the benchmark).

---

## 3. The resource manager: `src/rm.c` + `include/rm.h`

This is the heart of the project. All state sits in `rm_t` behind **one mutex and one condition variable**:

```
available[j]      free units of resource j
max[i][j]         declared claim of request i        (Banker's Max)
alloc[i][j]       units request i holds               (Allocation)
request[i][j]     what request i is blocked waiting for right now (detection)
need = max - alloc   derived, never stored
```

Read `rm_acquire_vec()`. Every strategy goes through this one function:
1. Reject a request beyond the declared claim (`RM_ERROR` → 500).
2. Lockorder only: reject a "downhill" request (`RM_ORDER` → 500).
3. Record `request[slot]`, then loop:
   - On shutdown → `RM_SHUTDOWN`; if aborted as a victim → `RM_ABORTED`.
   - `can_grant_locked()` asks the active strategy whether to grant. If yes, move units from `available` to `alloc` and return.
   - Otherwise wait on the condvar. Banker and lockorder use `pthread_cond_timedwait` with a deadline (→ 503). Naive and detect use a plain `pthread_cond_wait`: a naive deadlock must stay a deadlock, and in detect mode only the detector may break it.
4. Every release does `pthread_cond_broadcast`. Waiters re-check in a loop, which handles spurious and lost wakeups.

`can_grant_locked()` is where the strategies differ, in a single `switch`:

| Mode | Rule | File |
|---|---|---|
| naive, lockorder, detect | `request ≤ available` | `src/naive.c` |
| banker | `banker_can_grant()`: request ≤ need, request ≤ available, **and the state after granting is safe** | `src/banker.c` |

---

## 4. Banker's algorithm: `src/banker.c`

`safety_scan()` is the textbook safety algorithm:

```
work = available (minus the tentative request)
repeat: find an unfinished request with  max - alloc <= work
        pretend it finishes:  work += alloc,  mark finished
safe  <=>  everyone finished
```

Two details worth pointing out:
- It scans only **active** slots, collected into `idx[]` first. A check costs O(k²·m) for k in-flight requests; `test_banker` measures about 1 µs (NFR-3 asks for < 100 µs).
- `banker_can_grant()` passes the tentative request in, rather than copying the 1024×8 tables.

**Why it works on the demo:**
1. `/refresh` holds DB (1 of 1).
2. `/upload` asks for CLOCK. Tentatively granting it leaves `available = {DB:0, CLOCK:0}`.
3. Now `/refresh` still needs CLOCK and `/upload` still needs DB. Nobody can finish, so the state is unsafe and the request is refused.
4. `/upload` waits. `/refresh` gets CLOCK, finishes, and releases both, and `/upload` proceeds.

**Fairness** (`should_yield_locked()` in `rm.c`): FIFO among waiters, plus aging. After `fairness_aging_ms`, new requests stand aside so a large claim isn't starved by a stream of small ones. The comment explains why the FIFO rule must skip waiters that aging is holding back: without that, two waiters yielded to each other forever. This was a real bug found in benchmarking (EDD §18).

---

## 5. The baselines

**Lock ordering** (`src/lockorder.c`):
- `lockorder_sort_plan()` reorders any handler's plan into the global index order (`WORKER < BUF < FD < DB < CLOCK`, the order of `resources.conf`).
- `lockorder_order_ok()` refuses any request for a type at or below the highest type already held.
- A wait-for cycle would need someone to go "downhill", so circular wait can't happen.

**Detection and recovery** (`src/detect.c`, `rm_detect_and_recover()` in `rm.c`):
- `detect_find_deadlocked()` is the multi-instance detection algorithm. It works like the safety algorithm but uses the **Request** matrix (what each blocked request wants now), not `Need`.
- A running request has `request = 0`, so it's never flagged.
- The detector thread wakes every `detect_interval_ms` and aborts the deadlocked request holding the fewest units. That request is always blocked when chosen, so taking back its resources is safe.
- It's woken with `RM_ABORTED` and its client gets `503`.

---

## 6. Observability: `src/metrics.c`, `www/index.html`

- `GET /metrics` returns JSON with:
  - per-status counters, rejections, timeouts and shed requests
  - p50/p95/p99 latency from a reservoir sample
  - Banker's check count and timing, plus how many grants were refused as unsafe
  - deadlocks found and victims aborted
  - the live allocation table per resource
- `--util-csv FILE` records units in use per resource every 100 ms (`metrics_start_sampler`).
- Open `http://localhost:8080/` for the dashboard. It polls `/metrics` and draws per-resource bars.

---

## 7. Tests: `tests/`

| File | Read it for |
|---|---|
| `test_banker.c` | textbook example; `brute_force_safe()` tries every ordering and is compared with Banker's on 20,000 random states |
| `test_detect.c` | textbook detection example, victim choice, lock-order rule |
| `test_modes.c` | the four strategies on the circular-wait workload; the starvation test |
| `test_rm_stress.c` | 24 threads, random acquisition orders; a checker thread asserts invariants every ms |
| `test_http.c` | malformed requests, percent-decoding, symlink escape |
| `e2e.sh` | the real server over HTTP, including "naive leaves requests unanswered" |

`make asan` and `make tsan` run the suites under AddressSanitizer and ThreadSanitizer.

---

## 8. Benchmarks: `tools/`

- `loadgen.c`: closed-loop clients with a fixed seed per client. Each request has one deadline covering connect, send and response, so a frozen server shows up as **timeouts**. Latency percentiles cover successes only.
- `bench.sh`: a fresh server for each strategy × mix × load level × repetition, appending to `results/bench.csv`.
- `plot.py`: `results/bench.png`, or `--markdown` for the summary table used in [`REPORT.md`](REPORT.md).

---

## 9. Where to go next

- [`REPORT.md`](REPORT.md): results and the trade-offs between the strategies.
- [`EDD.md`](EDD.md) §18: every place the implementation differs from the original design, and why.
- Things to try:
  - `./bin/safeserve --mode naive`, then fire conflicting requests and watch `/metrics` show the stuck allocation table.
  - Set `fairness_aging_ms = 0` and rerun `make test` to see the starvation case.
  - Change the order in `handle_upload` to match `/refresh`; naive mode stops deadlocking.
