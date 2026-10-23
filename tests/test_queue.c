#define _POSIX_C_SOURCE 200809L
#include "queue.h"
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#define NUM_PRODUCERS 4
#define NUM_CONSUMERS 4
#define ITEMS_PER_PRODUCER 500

static queue_t test_q;
static atomic_int items_consumed = 0;

static void *producer_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        job_t job;
        job.client_fd = 100 + i;
        clock_gettime(CLOCK_MONOTONIC, &job.t_accept);

        // Keep trying until accepted
        while (!queue_try_push(&test_q, job)) {
            struct timespec ts = {0, 100000}; // 100 us
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void *consumer_fn(void *arg) {
    (void)arg;
    job_t job;
    while (queue_pop(&test_q, &job)) {
        atomic_fetch_add(&items_consumed, 1);
    }
    return NULL;
}

static void test_single_threaded(void) {
    queue_t q;
    assert(queue_init(&q, 2) == 0);

    job_t j1 = {.client_fd = 1};
    job_t j2 = {.client_fd = 2};
    job_t j3 = {.client_fd = 3};

    assert(queue_try_push(&q, j1) == true);
    assert(queue_try_push(&q, j2) == true);
    assert(queue_try_push(&q, j3) == false); // Queue is full

    job_t popped;
    assert(queue_pop(&q, &popped) == true);
    assert(popped.client_fd == 1);

    assert(queue_try_push(&q, j3) == true); // Now has space

    assert(queue_pop(&q, &popped) == true);
    assert(popped.client_fd == 2);
    assert(queue_pop(&q, &popped) == true);
    assert(popped.client_fd == 3);

    queue_destroy(&q);
    printf("[test_queue] Single-threaded push/pop/full tests: PASS\n");
}

static void test_multithreaded_mpmc(void) {
    assert(queue_init(&test_q, 64) == 0);

    pthread_t prod[NUM_PRODUCERS];
    pthread_t cons[NUM_CONSUMERS];

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        pthread_create(&cons[i], NULL, consumer_fn, NULL);
    }
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_create(&prod[i], NULL, producer_fn, NULL);
    }

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(prod[i], NULL);
    }

    // Give consumers a moment to drain
    while (queue_count(&test_q) > 0) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    queue_close(&test_q);

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        pthread_join(cons[i], NULL);
    }

    int expected = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    int actual = atomic_load(&items_consumed);
    printf("[test_queue] Multithreaded MPMC consumed %d / %d items: %s\n",
           actual, expected, (actual == expected) ? "PASS" : "FAIL");
    assert(actual == expected);

    queue_destroy(&test_q);
}

int main(void) {
    printf("=== Running Queue Unit Tests ===\n");
    test_single_threaded();
    test_multithreaded_mpmc();
    printf("=== All Queue Tests Passed! ===\n");
    return 0;
}
