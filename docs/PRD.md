# SafeServe: Product Requirements Document (PRD)

**Project:** SafeServe, a multi-threaded web server with deadlock-safe admission control
**Course:** Operating Systems project
**Status:** v1.1 (implemented)
**Last updated:** 2026-10-23

---

## 1. Overview

SafeServe is a multi-threaded HTTP/1.1 server written in C (POSIX threads). Each request may need several limited resources at the same time: worker slots, buffer memory, file descriptors, database connections, and cache write locks. When handlers acquire these resources one by one with no coordination, concurrent requests can deadlock and freeze the server.

SafeServe places an **admission controller** in front of the worker pool. The controller applies the **Banker's algorithm** and lets a request proceed only if the system stays in a *safe state*. Otherwise the request waits briefly or is rejected with `503 Service Unavailable`.

The project is both a working server and a comparative study of three deadlock-handling strategies: **prevention**, **detection and recovery**, and **avoidance**.

## 2. Problem Statement

Servers that hold multiple resources per request can hit circular wait. For example, request A holds a DB connection and waits for a cache lock, while request B holds the cache lock and waits for a DB connection. Both stall forever, and the stall spreads until the whole server is unresponsive.

Common fixes have costs of their own:
- **Prevention** (for example, lock ordering) can be hard to apply and can reduce concurrency.
- **Detection and recovery** lets deadlocks happen and then kills a victim request, which wastes work.
- **Avoidance** needs advance knowledge of maximum resource needs and can be conservative.

We need a working system that shows these trade-offs with measurements.

## 3. Goals

| ID | Goal |
|---|---|
| G1 | Serve HTTP requests concurrently using a fixed thread pool. |
| G2 | Guarantee **zero deadlocks** under any workload that respects declared resource claims. |
| G3 | Degrade gracefully under overload (bounded latency, explicit `503 Retry-After`). |
| G4 | Provide a reproducible demo where a naive server deadlocks and SafeServe does not. |
| G5 | Provide quantitative comparison of four strategies: naive, lock-ordering, detection and recovery, and Banker's avoidance. |
| G6 | Demonstrate core OS concepts: threads, mutexes, condition variables, scheduling, deadlock handling, resource management. |

## 4. Non-Goals

- HTTPS/TLS, HTTP/2, HTTP/3, WebSockets.
- Production hardening, authentication, full HTTP spec compliance.
- Distributed deployment or multi-process clustering.
- Windows support (target is Linux).

## 5. Stakeholders and Users

| Role | Interest |
|---|---|
| Course instructor / evaluators | Correct use of OS concepts, quality of evaluation, clear report. |
| Project team | A buildable, testable, demonstrable system. |
| Demo audience | A clear before/after view of deadlock vs. no deadlock. |

**Primary "user" personas**
- **Operator:** configures resource pools and route claims, starts the server, reads metrics.
- **Load tester:** runs workloads and compares strategies.

## 6. Functional Requirements

### 6.1 HTTP serving
| ID | Requirement | Priority |
|---|---|---|
| FR-1 | Accept TCP connections on a configurable port. | Must |
| FR-2 | Parse HTTP/1.1 `GET` and `POST` requests (request line, headers, body by `Content-Length`). | Must |
| FR-3 | Serve static files from a configured document root, with correct `Content-Type` and status codes (200, 400, 404, 405, 500, 503). | Must |
| FR-4 | Support routes: `/static/*`, `/upload`, `/query`, `/refresh`, `/metrics`, `/health`. | Must |
| FR-5 | Reject path traversal (`..`) attempts. | Must |
| FR-6 | Support HTTP keep-alive. | Could |

### 6.2 Concurrency
| ID | Requirement | Priority |
|---|---|---|
| FR-7 | Use one acceptor thread and a fixed-size worker pool fed by a bounded queue. | Must |
| FR-8 | When the queue is full, reject immediately with `503`. | Must |
| FR-9 | Shut down gracefully on `SIGINT`/`SIGTERM` (stop accepting, drain in-flight requests, join threads). | Must |

### 6.3 Resource management and admission control
| ID | Requirement | Priority |
|---|---|---|
| FR-10 | Define resource types and instance counts in a config file. | Must |
| FR-11 | Define each route's **maximum resource claim** in a config file. | Must |
| FR-12 | Maintain `Available`, `Max`, `Allocation`, `Need` tables under synchronization. | Must |
| FR-13 | Run the Banker's **safety algorithm** before granting any resource request. | Must |
| FR-14 | Grant a request only if the resulting state is safe; otherwise block the requester until a release makes it safe. | Must |
| FR-15 | Reject requests whose claim exceeds total system capacity or exceeds their own declared claim. | Must |
| FR-16 | Time out waiting requests (configurable) and return `503` with `Retry-After`. | Must |
| FR-17 | Prevent starvation of large claims (FIFO or aging in the wait queue). | Should |
| FR-18 | Support two admission modes: **atomic reservation** (reserve full claim up front) and **incremental** (handlers request resources step by step with a safety check at each step). | Should |

### 6.4 Baselines (for evaluation)
| ID | Requirement | Priority |
|---|---|---|
| FR-19 | **Naive mode:** incremental acquisition with no deadlock handling. | Must |
| FR-20 | **Lock-ordering mode:** resources acquired in a global fixed order (prevention). | Must |
| FR-21 | **Detection mode:** maintain a wait-for graph, detect cycles periodically, abort a victim request with `500`/`503`. | Must |
| FR-22 | **Banker's mode:** the SafeServe default. | Must |
| FR-23 | Select the mode at startup via config or CLI flag. | Must |

### 6.5 Observability
| ID | Requirement | Priority |
|---|---|---|
| FR-24 | Log each request: timestamp, route, status, latency, queue wait, admission wait. | Must |
| FR-25 | Expose `/metrics` (JSON or plain text): requests served, rejections, timeouts, queue length, p50/p95/p99 latency, safety checks run, current allocation table. | Should |
| FR-26 | Produce CSV output suitable for plotting. | Should |

## 7. Non-Functional Requirements

| ID | Category | Requirement |
|---|---|---|
| NFR-1 | Correctness | No data races (clean under ThreadSanitizer/Helgrind) on supported workloads. |
| NFR-2 | Safety | SafeServe mode never deadlocks in a stress run of at least 10 minutes at 1000 concurrent clients. |
| NFR-3 | Performance | Safety-check overhead per request under 100 µs for up to 8 resource types and 64 concurrent requests. |
| NFR-4 | Performance | Throughput within a reasonable margin of the lock-ordering baseline (target: within 25% on mixed workloads). |
| NFR-5 | Robustness | No crashes or leaks on malformed input (checked with Valgrind/ASan). |
| NFR-6 | Portability | Builds with `gcc` and `make` on Linux with no external dependencies beyond libc and pthreads. |
| NFR-7 | Reproducibility | Deadlock demo and benchmarks run from documented scripts with fixed seeds. |
| NFR-8 | Maintainability | Modular code: networking, HTTP, resource manager, handlers, metrics are separate modules. |

## 8. Success Metrics

| Metric | Target |
|---|---|
| Deadlocks in SafeServe mode | 0 over all stress runs |
| Naive baseline deadlock reproduced | Yes, deterministically via demo script |
| Safety algorithm unit tests | 100% pass, including textbook examples |
| Admission overhead | Reported and below NFR-3 target |
| Tail latency under overload | p99 bounded by configured admission timeout plus service time |
| Evaluation completeness | All four strategies measured on at least 3 workload mixes and 3 concurrency levels |

## 9. Scope Summary

**In scope:** HTTP server, thread pool, resource manager, Banker's admission, three baselines, load generator, benchmarks, report, demo.

**Out of scope:** TLS, HTTP/2, authentication, production deployment.

**Stretch (only if time allows):** priority classes, learned claim profiles, live safe-state visualization, `epoll` comparison.

## 10. Milestones

| Week | Milestone | Exit criteria |
|---|---|---|
| 1 | Scope and skeleton | Single-threaded server serves static files; resource model defined |
| 2 | Concurrency | Thread pool and bounded queue; logging |
| 3 | Banker's module | Safety algorithm passes unit tests |
| 4 | Integration | Admission controller wired in; 503 path; naive deadlock reproduced |
| 5 | Baselines and benchmarks | Lock-ordering and detection modes; load tests run |
| 6 | Wrap-up | Plots, report, demo, cleanup |

## 11. Deliverables

1. Source code (C, pthreads, Makefile) with README.
2. Config files for resource pools and route claims.
3. Deadlock demo script (naive vs. SafeServe).
4. Load generator and benchmark scripts, raw CSVs, and plots.
5. Final report: problem, design, algorithm, evaluation, limitations.
6. Presentation and live demo.

## 12. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Banker's is over-conservative and lowers utilization | Medium | Medium | Tune claims; report the effect honestly; compare against baselines |
| Races in the resource manager | Medium | High | Single global mutex first; run TSan/Helgrind |
| Naive deadlock is hard to reproduce | Medium | Medium | Deterministic workload with fixed order and artificial delays |
| Scope creep (TLS, HTTP/2) | High | Medium | Explicitly out of scope |
| Benchmark noise | Medium | Medium | Repeat runs, report median and spread, pin CPU where possible |
| Claims declared too low, causing unsafe behavior | Low | High | Enforce claims at runtime; reject violations |

## 13. Assumptions and Constraints

- Each route's maximum resource usage is known ahead of time (required by Banker's).
- Resource counts are small and fixed at startup.
- Development and testing happen on Linux.
- The team is a small student group working roughly six weeks.

## 14. Acceptance Criteria

The project is accepted when:
1. All **Must** requirements pass their tests.
2. The demo script shows the naive server freezing and SafeServe continuing under the same workload.
3. Benchmark results cover all four strategies and are included in the report.
4. The code builds cleanly with `make` and is race-free under ThreadSanitizer on the standard test suite.

## 15. Open Questions

1. Final team size and deadline (affects stretch goals). *Open.*
2. Which admission mode is the default? **Resolved: incremental.** Atomic reservation removes hold-and-wait, so it cannot show the difference between strategies. See EDD §17.
3. Real SQLite or simulated DB? **Resolved: simulated** with a configurable delay.
