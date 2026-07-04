// test_producer_consumer.c — producer/consumer test for bifrost-emu.
//
// Classic concurrency pattern: N producers push items into a bounded buffer,
// M consumers pull items out. Tests:
//   - pthread_mutex + pthread_cond coordination
//   - Bounded buffer (ring buffer) with full/empty condition variables
//   - Correct item accounting (no lost or duplicate items)
//   - Multiple producers and consumers running concurrently
//
// NOTE: Uses conservative scale (2 producers + 2 consumers x 200 items) to
// stay within the emulator's current futex-wakeup throughput.
//
// Build: make cross SRC=ctest/test_producer_consumer.c OUT=ctest/test_producer_consumer.elf
// Run:   ./bifrost-emu ctest/test_producer_consumer.elf
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPROD    2
#define NCONS    2
#define NITEMS   200
#define BUFSIZE  8

typedef struct {
    int buf[BUFSIZE];
    int in;
    int out;
    int count;
    int items_remaining;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} bbuf_t;

static bbuf_t bb;
static long consumed_sum = 0;
static int consumed_count = 0;

static void bb_init(bbuf_t* b) {
    memset(b, 0, sizeof(*b));
    b->items_remaining = NPROD * NITEMS;
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_full, NULL);
    pthread_cond_init(&b->not_empty, NULL);
}

typedef struct { int id; } targ_t;

static void* producer(void* arg) {
    targ_t* a = (targ_t*)arg;
    int base = a->id * NITEMS;
    for (int i = 0; i < NITEMS; i++) {
        pthread_mutex_lock(&bb.lock);
        while (bb.count == BUFSIZE) {
            pthread_cond_wait(&bb.not_full, &bb.lock);
        }
        bb.buf[bb.in] = base + i;
        bb.in = (bb.in + 1) % BUFSIZE;
        bb.count++;
        bb.items_remaining--;
        pthread_cond_signal(&bb.not_empty);
        pthread_mutex_unlock(&bb.lock);
    }
    // Wake any blocked consumers.
    pthread_cond_broadcast(&bb.not_empty);
    return NULL;
}

static void* consumer(void* arg) {
    targ_t* a = (targ_t*)arg;
    (void)a;
    for (;;) {
        pthread_mutex_lock(&bb.lock);
        while (bb.count == 0 && bb.items_remaining > 0) {
            pthread_cond_wait(&bb.not_empty, &bb.lock);
        }
        int done = (bb.count == 0 && bb.items_remaining == 0);
        if (done) {
            pthread_mutex_unlock(&bb.lock);
            break;
        }
        int item = bb.buf[bb.out];
        bb.out = (bb.out + 1) % BUFSIZE;
        bb.count--;
        pthread_cond_signal(&bb.not_full);
        pthread_mutex_unlock(&bb.lock);
        __sync_fetch_and_add(&consumed_sum, item);
        __sync_fetch_and_add(&consumed_count, 1);
    }
    return NULL;
}

int main(void) {
    printf("=== producer/consumer: %d producers + %d consumers x %d items ===\n",
           NPROD, NCONS, NITEMS);
    bb_init(&bb);

    pthread_t prods[NPROD], cons[NCONS];
    targ_t pargs[NPROD], cargs[NCONS];
    for (int i = 0; i < NPROD; i++) {
        pargs[i].id = i;
        pthread_create(&prods[i], NULL, producer, &pargs[i]);
    }
    for (int i = 0; i < NCONS; i++) {
        cargs[i].id = i;
        pthread_create(&cons[i], NULL, consumer, &cargs[i]);
    }
    for (int i = 0; i < NPROD; i++) pthread_join(prods[i], NULL);
    for (int i = 0; i < NCONS; i++) pthread_join(cons[i], NULL);

    long expected_count = (long)NPROD * NITEMS;
    long expected_sum = 0;
    for (int p = 0; p < NPROD; p++) {
        for (int i = 0; i < NITEMS; i++) {
            expected_sum += p * NITEMS + i;
        }
    }

    int count_ok = (consumed_count == expected_count);
    int sum_ok = (consumed_sum == expected_sum);
    printf("consumed_count = %ld (expected %ld)  %s\n",
           consumed_count, expected_count, count_ok ? "OK" : "FAIL");
    printf("consumed_sum = %ld (expected %ld)  %s\n",
           consumed_sum, expected_sum, sum_ok ? "OK" : "FAIL");

    pthread_mutex_destroy(&bb.lock);
    pthread_cond_destroy(&bb.not_full);
    pthread_cond_destroy(&bb.not_empty);

    int ok = count_ok && sum_ok;
    printf("test_producer_consumer: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
