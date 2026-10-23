#define _POSIX_C_SOURCE 200809L
#include "banker.h"
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_classic_textbook_case(void) {
    // Silberschatz textbook Banker's Example:
    // 5 processes (P0 - P4), 3 resource types (A=10, B=5, C=7)
    int n = 5;
    int m = 3;

    int available[M_MAX] = {3, 3, 2};

    int alloc[N_MAX][M_MAX] = {
        {0, 1, 0}, // P0
        {2, 0, 0}, // P1
        {3, 0, 2}, // P2
        {2, 1, 1}, // P3
        {0, 0, 2}  // P4
    };

    int max_claim[N_MAX][M_MAX] = {
        {7, 5, 3}, // P0
        {3, 2, 2}, // P1
        {9, 0, 2}, // P2
        {2, 2, 2}, // P3
        {4, 3, 3}  // P4
    };

    bool active[N_MAX] = {true, true, true, true, true};

    // Test 1: Verify initial state is safe (safe sequence: P1, P3, P4, P0, P2)
    bool safe = banker_is_safe(available, alloc, max_claim, active, n, m);
    printf("[test_banker] Initial textbook state is safe: %s\n", safe ? "PASS" : "FAIL");
    assert(safe == true);

    // Test 2: P1 requests (1, 0, 2) -> Should be granted safely
    int req_p1[M_MAX] = {1, 0, 2};
    bool can_grant_p1 = banker_can_grant(available, alloc, max_claim, active, n, m, 1, req_p1);
    printf("[test_banker] P1 requests (1, 0, 2) can grant: %s\n", can_grant_p1 ? "PASS" : "FAIL");
    assert(can_grant_p1 == true);

    // Test 3: P4 requests (3, 3, 0) -> Exceeds available (3, 3, 2) since available[2] is 2, but avail[0]=3, avail[1]=3
    // But wait! Even if P0 requests (0, 2, 0), does that leave system in safe state?
    // Let's test a request that exceeds available: P0 requests (4, 0, 0) -> available is 3
    int req_excess[M_MAX] = {4, 0, 0};
    bool can_grant_excess = banker_can_grant(available, alloc, max_claim, active, n, m, 0, req_excess);
    printf("[test_banker] P0 requests (4, 0, 0) exceeds available: %s\n", !can_grant_excess ? "PASS" : "FAIL");
    assert(can_grant_excess == false);

    // Test 4: Request that exceeds declared claim: P1 has max (3, 2, 2) and alloc (2, 0, 0) -> need is (1, 2, 2).
    // Request (2, 0, 0) exceeds need (1, 2, 2)
    int req_claim_violation[M_MAX] = {2, 0, 0};
    bool can_grant_violation = banker_can_grant(available, alloc, max_claim, active, n, m, 1, req_claim_violation);
    printf("[test_banker] P1 exceeds claim rejection: %s\n", !can_grant_violation ? "PASS" : "FAIL");
    assert(can_grant_violation == false);

    // Test 5: the safe sequence found is the textbook one <P1, P3, P4, P0, P2>
    int seq[N_MAX];
    int len = banker_safe_sequence(available, alloc, max_claim, active, n, m, seq);
    int expected_seq[5] = {1, 3, 4, 0, 2};
    assert(len == 5);
    assert(memcmp(seq, expected_seq, sizeof(expected_seq)) == 0);
    printf("[test_banker] Safe sequence <P1, P3, P4, P0, P2>: PASS\n");

    // Test 6 (textbook follow-up): after granting P1 (1,0,2), available is (2,3,0).
    // P4 asking (3,3,0) must wait (not enough free); P0 asking (0,2,0) has the
    // units free but would leave the system unsafe.
    for (int j = 0; j < m; j++) {
        available[j] -= req_p1[j];
        alloc[1][j] += req_p1[j];
    }
    assert(banker_is_safe(available, alloc, max_claim, active, n, m));
    int req_p4[M_MAX] = {3, 3, 0};
    int req_p0[M_MAX] = {0, 2, 0};
    assert(!banker_can_grant(available, alloc, max_claim, active, n, m, 4, req_p4));
    assert(!banker_can_grant(available, alloc, max_claim, active, n, m, 0, req_p0));
    printf("[test_banker] Textbook follow-ups (P4 waits, P0 unsafe): PASS\n");
}

static void test_boundaries(void) {
    int available[M_MAX] = {4};
    int alloc[N_MAX][M_MAX] = {{0}};
    int max_claim[N_MAX][M_MAX] = {{4}, {0}};
    bool active[N_MAX] = {true, true};

    // A claim equal to the total is safe: it can run alone.
    assert(banker_is_safe(available, alloc, max_claim, active, 2, 1));
    int all[M_MAX] = {4};
    assert(banker_can_grant(available, alloc, max_claim, active, 2, 1, 0, all));

    // Zero-need slot always finishes; zero request is always grantable.
    int zero[M_MAX] = {0};
    assert(banker_can_grant(available, alloc, max_claim, active, 2, 1, 1, zero));

    // A request for an inactive slot is refused.
    active[1] = false;
    assert(!banker_can_grant(available, alloc, max_claim, active, 2, 1, 1, zero));

    // Inactive slots are ignored by the safety check.
    int alloc2[N_MAX][M_MAX] = {{1}, {1}};
    int max2[N_MAX][M_MAX] = {{2}, {9}};
    int avail2[M_MAX] = {0};
    bool active2[N_MAX] = {true, false};
    assert(!banker_is_safe(avail2, alloc2, max2, active2, 2, 1));
    avail2[0] = 1;
    assert(banker_is_safe(avail2, alloc2, max2, active2, 2, 1));

    printf("[test_banker] Boundary cases (claim == total, zero need, inactive): PASS\n");
}

// ---- Property test: compare against brute force over every ordering ----

static bool brute_force_safe(const int *avail, int alloc[][M_MAX], int max_claim[][M_MAX],
                             bool *finished, int n, int m, int remaining) {
    if (remaining == 0) return true;
    for (int i = 0; i < n; i++) {
        if (finished[i]) continue;
        bool fits = true;
        for (int j = 0; j < m; j++) {
            if (max_claim[i][j] - alloc[i][j] > avail[j]) {
                fits = false;
                break;
            }
        }
        if (!fits) continue;
        int next[M_MAX];
        for (int j = 0; j < m; j++) next[j] = avail[j] + alloc[i][j];
        finished[i] = true;
        bool ok = brute_force_safe(next, alloc, max_claim, finished, n, m, remaining - 1);
        finished[i] = false;
        if (ok) return true;
    }
    return false;
}

static void test_property_random(void) {
    srand(42); // Fixed seed: reproducible (NFR-7)
    int trials = 20000, safe_count = 0, grant_checks = 0;

    for (int t = 0; t < trials; t++) {
        int n = 1 + rand() % 6;
        int m = 1 + rand() % 3;
        int total[M_MAX], available[M_MAX];
        int alloc[N_MAX][M_MAX] = {{0}};
        int max_claim[N_MAX][M_MAX] = {{0}};
        bool active[N_MAX] = {false};

        for (int j = 0; j < m; j++) {
            total[j] = 1 + rand() % 8;
            available[j] = total[j];
        }
        for (int i = 0; i < n; i++) {
            active[i] = true;
            for (int j = 0; j < m; j++) {
                max_claim[i][j] = rand() % (total[j] + 1);
                int a = max_claim[i][j] ? rand() % (max_claim[i][j] + 1) : 0;
                if (a > available[j]) a = available[j];
                alloc[i][j] = a;
                available[j] -= a;
            }
        }

        bool finished[N_MAX] = {false};
        bool expect = brute_force_safe(available, alloc, max_claim, finished, n, m, n);
        bool got = banker_is_safe(available, alloc, max_claim, active, n, m);
        if (expect != got) {
            printf("[test_banker] Mismatch on trial %d: brute=%d banker=%d\n", t, expect, got);
        }
        assert(expect == got);
        if (got) safe_count++;

        // banker_can_grant must equal "request fits and the resulting state is safe".
        int slot = rand() % n;
        int req[M_MAX];
        bool fits = true;
        for (int j = 0; j < m; j++) {
            int need = max_claim[slot][j] - alloc[slot][j];
            req[j] = need > 0 ? rand() % (need + 1) : 0;
            if (req[j] > available[j]) fits = false;
        }
        bool expect_grant = false;
        if (fits) {
            int avail2[M_MAX];
            for (int j = 0; j < m; j++) {
                avail2[j] = available[j] - req[j];
                alloc[slot][j] += req[j];
            }
            bool fin2[N_MAX] = {false};
            expect_grant = brute_force_safe(avail2, alloc, max_claim, fin2, n, m, n);
            for (int j = 0; j < m; j++) alloc[slot][j] -= req[j];
        }
        assert(banker_can_grant(available, alloc, max_claim, active, n, m, slot, req) == expect_grant);
        grant_checks++;
    }

    printf("[test_banker] Property test vs brute force: %d states (%d safe), %d grant checks: PASS\n",
           trials, safe_count, grant_checks);
}

static void test_unsafe_detection(void) {
    // 2 processes, 1 resource type with 2 instances
    int n = 2;
    int m = 1;
    int available[M_MAX] = {0};
    int alloc[N_MAX][M_MAX] = {
        {1},
        {1}
    };
    int max_claim[N_MAX][M_MAX] = {
        {2},
        {2}
    };
    bool active[N_MAX] = {true, true};

    // Both hold 1, both need 1 more, 0 available => Deadlocked / Unsafe!
    bool safe = banker_is_safe(available, alloc, max_claim, active, n, m);
    printf("[test_banker] Unsafe circular wait detected: %s\n", !safe ? "PASS" : "FAIL");
    assert(safe == false);
}

// NFR-3: a safety check must cost < 100 us with 8 resource types and 64
// concurrent requests. Measured on the real table size (N_MAX slots).
static void test_overhead_nfr3(void) {
    static int alloc[N_MAX][M_MAX], max_claim[N_MAX][M_MAX];
    static bool active[N_MAX];
    int available[M_MAX];
    int m = 8, k = 64;
    srand(7);
    for (int j = 0; j < m; j++) available[j] = 4;
    for (int i = 0; i < k; i++) {
        int slot = i * (N_MAX / k); // Spread over the table, as in a long-running server
        active[slot] = true;
        for (int j = 0; j < m; j++) {
            alloc[slot][j] = rand() % 2;
            max_claim[slot][j] = alloc[slot][j] + rand() % 3;
        }
    }
    int req[M_MAX] = {1, 0, 0, 0, 0, 0, 0, 0};
    int target = 0;
    while (max_claim[target][0] - alloc[target][0] < 1) target += N_MAX / k;

    int iters = 20000;
    volatile bool sink = false;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int it = 0; it < iters; it++) {
        sink ^= banker_can_grant(available, alloc, max_claim, active, N_MAX, m, target, req);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void)sink;
    double us = ((double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3) / iters;
    printf("[test_banker] NFR-3 safety check, 64 requests x 8 types: %.2f us (< 100 us): %s\n",
           us, us < 100.0 ? "PASS" : "FAIL");
    assert(us < 100.0);
}

int main(void) {
    printf("=== Running Banker's Algorithm Unit Tests ===\n");
    test_classic_textbook_case();
    test_unsafe_detection();
    test_boundaries();
    test_property_random();
    test_overhead_nfr3();
    printf("=== All Banker's Algorithm Tests Passed! ===\n");
    return 0;
}
