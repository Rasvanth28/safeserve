#define _POSIX_C_SOURCE 200809L
#include "detect.h"
#include "lockorder.h"
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>

// Silberschatz §8.7.2 detection example: 5 processes, A=7, B=2, C=6.
static void test_textbook_detection(void) {
    int n = 5, m = 3;
    int available[M_MAX] = {0, 0, 0};
    int alloc[N_MAX][M_MAX] = {
        {0, 1, 0}, {2, 0, 0}, {3, 0, 3}, {2, 1, 1}, {0, 0, 2}
    };
    int request[N_MAX][M_MAX] = {
        {0, 0, 0}, {2, 0, 2}, {0, 0, 0}, {1, 0, 0}, {0, 0, 2}
    };
    bool active[N_MAX] = {true, true, true, true, true};
    bool dl[N_MAX];

    int count = detect_find_deadlocked(available, alloc, request, active, n, m, dl);
    printf("[test_detect] Textbook state is deadlock-free: %s\n", count == 0 ? "PASS" : "FAIL");
    assert(count == 0);

    // P2 now asks for one more C: P1..P4 are deadlocked, P0 is not.
    request[2][2] = 1;
    count = detect_find_deadlocked(available, alloc, request, active, n, m, dl);
    printf("[test_detect] After P2 requests (0,0,1), P1-P4 deadlocked: %s\n",
           (count == 4 && !dl[0] && dl[1] && dl[2] && dl[3] && dl[4]) ? "PASS" : "FAIL");
    assert(count == 4 && !dl[0] && dl[1] && dl[2] && dl[3] && dl[4]);
}

static void test_running_slots_not_deadlocked(void) {
    // A slot that holds resources but is not waiting (request = 0) is running
    // and will release them: never part of a deadlock, even with need left.
    int available[M_MAX] = {0};
    int alloc[N_MAX][M_MAX] = {{1}, {1}};
    int request[N_MAX][M_MAX] = {{0}, {1}};
    bool active[N_MAX] = {true, true};
    bool dl[N_MAX];
    assert(detect_find_deadlocked(available, alloc, request, active, 2, 1, dl) == 0);

    // Inactive slots are ignored, whatever stale values their rows hold.
    int alloc2[N_MAX][M_MAX] = {{5}, {1}};
    int request2[N_MAX][M_MAX] = {{7}, {1}};
    int available2[M_MAX] = {1};
    active[0] = false;
    assert(detect_find_deadlocked(available2, alloc2, request2, active, 2, 1, dl) == 0);
    active[0] = true;
    assert(detect_find_deadlocked(available, alloc, request, active, 2, 1, dl) == 0);
    request[0][0] = 1; // Now both wait on each other
    assert(detect_find_deadlocked(available, alloc, request, active, 2, 1, dl) == 2);
    printf("[test_detect] Running and inactive slots are never deadlocked: PASS\n");
}

static void test_victim_selection(void) {
    int alloc[N_MAX][M_MAX] = {{3, 1}, {1, 0}, {1, 0}, {0, 5}};
    uint64_t ticket[N_MAX] = {1, 2, 3, 4};
    bool dl[N_MAX] = {true, true, true, false};

    // Fewest held units wins; tie between slots 1 and 2 -> youngest (slot 2).
    int v = detect_pick_victim(alloc, ticket, dl, 4, 2);
    printf("[test_detect] Victim = fewest held, youngest on tie: %s\n", v == 2 ? "PASS" : "FAIL");
    assert(v == 2);

    bool none[N_MAX] = {false};
    assert(detect_pick_victim(alloc, ticket, none, 4, 2) == -1);
}

static void test_lockorder_rules(void) {
    int held_none[M_MAX] = {0, 0, 0, 0, 0};
    int held_db[M_MAX] = {0, 0, 0, 1, 0};
    int want_clock[M_MAX] = {0, 0, 0, 0, 1};
    int want_db[M_MAX] = {0, 0, 0, 1, 0};
    int want_buf_and_clock[M_MAX] = {0, 2, 0, 0, 1};

    assert(lockorder_order_ok(held_none, want_db, 5));
    assert(lockorder_order_ok(held_db, want_clock, 5));          // DB(3) -> CLOCK(4): uphill
    assert(!lockorder_order_ok(held_db, want_db, 5));            // same type again: refused
    assert(!lockorder_order_ok(held_db, want_buf_and_clock, 5)); // BUF(1) is downhill

    acq_step_t plan[] = { {4, 1}, {1, 2}, {3, 1}, {1, 2}, {0, 1} };
    int n = lockorder_sort_plan(plan, 5);
    assert(n == 4);
    assert(plan[0].res == 0 && plan[1].res == 1 && plan[1].count == 4);
    assert(plan[2].res == 3 && plan[3].res == 4);
    printf("[test_detect] Lock-order rule and plan sorting: PASS\n");
}

int main(void) {
    printf("=== Running Detection & Lock-Order Unit Tests ===\n");
    test_textbook_detection();
    test_running_slots_not_deadlocked();
    test_victim_selection();
    test_lockorder_rules();
    printf("=== All Detection & Lock-Order Tests Passed! ===\n");
    return 0;
}
