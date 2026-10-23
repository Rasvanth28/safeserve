# SafeServe: Final Report

**Project:** SafeServe, a multi-threaded web server with deadlock-safe admission control
**Course:** Operating Systems
**Companions:** [`PRD.md`](PRD.md) (requirements), [`EDD.md`](EDD.md) (design, with as-built notes in §18)

---

## 1. Problem

A request to SafeServe needs several limited resources at the same time: a worker slot, buffer memory, file descriptors, a database connection and a cache write lock. When handlers take them one at a time and hold what they have while waiting for more, all four Coffman conditions can hold at once: mutual exclusion, hold-and-wait, no preemption and circular wait. The server can then deadlock.

The two routes at the centre of the study take the same pair of resources in opposite orders:

| Route | Order |
|---|---|
| `POST /upload` | … → `CLOCK` → `DB` |
| `POST /refresh` | … → `DB` → `CLOCK` |

With 4 DB connections and 2 cache locks, two uploads and four refreshes are enough. Each upload holds a `CLOCK` and waits for a `DB`; each refresh holds a `DB` and waits for a `CLOCK`.

## 2. Strategies compared

All four strategies share one set of resource tables (`Available`, `Max`, `Allocation`, `Request`) behind one mutex and condition variable (`src/rm.c`). They differ only in the grant rule:

| Mode | Coffman condition attacked | Grant rule | Waiting |
|---|---|---|---|
| `naive` | none | units free | forever |
| `lockorder` | circular wait | units free **and** requested type's index > every held type's index | bounded (`admit_timeout_ms`) → 503 |
| `detect` | none up front; recovers afterwards | units free | until granted or aborted; a detector thread aborts victims |
| `banker` | none; *avoids* unsafe states | units free **and** the state after the grant is safe | bounded → 503 |
| `banker-atomic` | hold-and-wait | whole claim at once, Banker's check | bounded → 503 |

**Banker's safety algorithm** (`src/banker.c`):
1. Set `work = Available`.
2. Repeatedly find an active request whose `Max − Allocation ≤ work`. Assume it finishes and returns its allocation: `work += Allocation`.
3. The state is safe iff every active request can finish this way.

The request algorithm tentatively grants and runs this check. Active requests are compacted first, so the cost is O(k²·m) for k in-flight requests rather than O(N_MAX²·m).

**Detection** (`src/detect.c`) is the multi-instance algorithm over the **Request** matrix. `request[i]` is non-zero only while request *i* is blocked. Requests that hold nothing, or whose current request fits in `work`, are assumed to finish. Whatever remains is deadlocked. The victim is the deadlocked request holding the fewest units (least work lost), with ties going to the youngest. It is always blocked when chosen, so taking back its resources never pulls them out from under running code.

**Fairness** (banker): FIFO among waiters plus aging. After `fairness_aging_ms`, new requests stand aside for the starved waiter (EDD §18).

## 3. Correctness evidence

| Claim | Evidence |
|---|---|
| Safety algorithm is correct | `test_banker`: Silberschatz textbook state (safe sequence ⟨P1, P3, P4, P0, P2⟩, the follow-up requests); boundary cases; **20,000 random states** compared with brute force over every ordering, for both `banker_is_safe` and `banker_can_grant`. |
| Detection is correct | `test_detect`: textbook detection example (deadlock-free, then P1–P4 deadlocked after P2's extra request); running and inactive slots never flagged; victim choice. |
| Naive really deadlocks; the others don't | `test_modes`, `make demo`, `make demo-live`, and the naive case in `tests/e2e.sh`, all on the opposite-order workload. |
| Invariants hold under concurrency | `test_rm_stress`: 24 threads × 60 requests with random acquisition order per mode. A checker thread asserts `0 ≤ available ≤ total`, `available + Σ alloc = total`, `alloc ≤ max`, and (banker) that the committed state is safe, every millisecond. |
| No data races / memory errors (NFR-1, NFR-5) | All unit tests under ThreadSanitizer, and the e2e suite plus 50-client load in three modes against a TSan-built server, with **no reports**. Unit and e2e tests under AddressSanitizer, also clean. |
| Starvation bounded (FR-17) | `test_modes`: a 4-unit claim competing with 12 threads of 1-unit claims. With aging it is granted in about 100 ms (asserted < 400 ms). Without aging it waited up to 470 ms in normal runs and timed out after 1.5 s under TSan. |
| Safety-check overhead (NFR-3) | `test_banker`: about **1 µs** per check with 64 in-flight requests × 8 resource types over the full 1,024-slot table (target < 100 µs). |

BENCHMARK_SECTION
