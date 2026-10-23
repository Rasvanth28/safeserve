#define _POSIX_C_SOURCE 200809L
#include "banker.h"
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>

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

int main(void) {
    printf("=== Running Banker's Algorithm Unit Tests ===\n");
    test_classic_textbook_case();
    test_unsafe_detection();
    printf("=== All Banker's Algorithm Tests Passed! ===\n");
    return 0;
}
