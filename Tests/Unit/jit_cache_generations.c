/* Code-cache generations. With a tiny budget (BIFROST_JIT_CACHE_MB=1) the shared JIT is replaced
 * many times while this program keeps threads in every state a renewal has to respect:
 *   - sleepers blocked in nanosleep/futex (the syscall runs from C++, no generated frame below),
 *   - a thread blocked in a futex wait inside a nested call (its generated frames pin the
 *     retired generation until it returns),
 *   - nested call loops that keep compiling while the instance they started on is retired,
 *   - rewritten code (instruction cache maintenance) across renewals,
 *   - signals taken by whichever thread, running generated code in the handler,
 *   - a fork after renewals.
 * Every result is checked. The driver script checks the generation counters. */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NFUN 3000
#define WORDS 100
typedef uint64_t (*fn_t)(uint64_t, uint64_t *);
typedef uint64_t (*driver_t)(uint64_t, unsigned);

static uint32_t *code;
static int failed;
static int long_flag, pinned_flag, pinned_in;
static volatile unsigned long signals_taken;

static void fail(const char *what) {
    if (!__atomic_exchange_n(&failed, 1, __ATOMIC_RELAXED)) fprintf(stderr, "FAIL: %s\n", what);
}

/* One of NFUN generated functions: 48 x (add x0,x0,#1; str x0,[x1]); returns seed+48. */
static uint64_t call_fun(unsigned i, uint64_t seed) {
    uint64_t value = seed;
    fn_t fn = (fn_t)(code + (size_t)(i % NFUN) * WORDS);
    uint64_t got = fn(seed, &value);
    if (got != seed + 48 || value != got) fail("generated function result");
    return got;
}

static long futex_wait(int *word, int expected) {
    return syscall(SYS_futex, word, FUTEX_WAIT, expected, NULL, NULL, 0);
}
static void futex_wake_all(int *word) { syscall(SYS_futex, word, FUTEX_WAKE, 1000, NULL, NULL, 0); }

/* A sleeper alternates a syscall that blocks with generated code that may be new each time. */
static void *sleeper(void *arg) {
    uint64_t seed = (uintptr_t)arg;
    for (unsigned k = 0; k < 150; k++) {
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
        for (unsigned i = 0; i < 40; i++) call_fun(k * 37 + i * 101, seed + k);
    }
    return NULL;
}

/* Blocked in one futex wait for most of the run, then computes with whatever JIT is current. */
static void *long_sleeper(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&long_flag, __ATOMIC_ACQUIRE)) futex_wait(&long_flag, 0);
    for (unsigned i = 0; i < NFUN; i += 3) call_fun(i, 77 + i);
    return NULL;
}

/* Called through a function pointer so the call is a nested native call (jit_call_helper). The
 * futex wait below then runs with generated frames of the generation it started on beneath it. */
static uint64_t pinned_driver(uint64_t seed, unsigned rounds) {
    __atomic_store_n(&pinned_in, 1, __ATOMIC_RELEASE);
    futex_wake_all(&pinned_in);
    while (!__atomic_load_n(&pinned_flag, __ATOMIC_ACQUIRE)) futex_wait(&pinned_flag, 0);
    uint64_t acc = 0;
    for (unsigned r = 0; r < rounds; r++) acc += call_fun(r * 11, seed + r);
    return acc;
}
static driver_t volatile pinned_ptr = pinned_driver;
static void *pinned_thread(void *arg) {
    (void)arg;
    uint64_t got = pinned_ptr(500, 200);
    uint64_t want = 0;
    for (unsigned r = 0; r < 200; r++) want += 500 + r + 48;
    if (got != want) fail("pinned driver result");
    return NULL;
}

/* A long nested call loop that compiles new code throughout: it must follow the current JIT. */
static uint64_t nested_driver(uint64_t seed, unsigned rounds) {
    uint64_t acc = 0;
    for (unsigned r = 0; r < rounds; r++)
        for (unsigned i = 0; i < NFUN; i += 5) acc += call_fun(i + r, seed + r);
    return acc;
}
static driver_t volatile nested_ptr = nested_driver;
static void *nested_thread(void *arg) {
    uint64_t seed = (uintptr_t)arg;
    uint64_t got = nested_ptr(seed, 6);
    uint64_t want = 0;
    for (unsigned r = 0; r < 6; r++)
        for (unsigned i = 0; i < NFUN; i += 5) want += seed + r + 48;
    if (got != want) fail("nested driver result");
    return NULL;
}

static void on_alarm(int sig) {
    (void)sig;
    signals_taken++;
    call_fun(5, 7);
}

/* movz x0,#value ; ret, rewritten and called repeatedly. */
static uint32_t *smc;
static void smc_check(unsigned value) {
    smc[0] = 0xd2800000u | ((value & 0xffffu) << 5);
    __builtin___clear_cache((char *)smc, (char *)(smc + 2));
    if (((uint64_t(*)(void))smc)() != value) fail("rewritten code");
}

int main(void) {
    code = mmap(NULL, (size_t)NFUN * WORDS * 4, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    smc = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || smc == MAP_FAILED) return 2;
    for (unsigned i = 0; i < NFUN; i++) {
        uint32_t *p = code + (size_t)i * WORDS;
        for (unsigned j = 0; j < 48; j++) { p[j * 2] = 0x91000400; p[j * 2 + 1] = 0xf9000020; }
        p[96] = 0xd65f03c0;
    }
    smc[1] = 0xd65f03c0;
    __builtin___clear_cache((char *)code, (char *)(code + (size_t)NFUN * WORDS));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval timer = {{0, 3000}, {0, 3000}};
    setitimer(ITIMER_REAL, &timer, NULL);

    pthread_t sleepers[3], long_t, pinned_t, nested_t[2];
    for (uintptr_t i = 0; i < 3; i++) pthread_create(&sleepers[i], NULL, sleeper, (void *)(1000 * (i + 1)));
    pthread_create(&long_t, NULL, long_sleeper, NULL);
    pthread_create(&pinned_t, NULL, pinned_thread, NULL);
    while (!__atomic_load_n(&pinned_in, __ATOMIC_ACQUIRE)) futex_wait(&pinned_in, 0);
    for (uintptr_t i = 0; i < 2; i++) pthread_create(&nested_t[i], NULL, nested_thread, (void *)(9000 * (i + 1)));

    /* Pressure: every pass compiles thousands of blocks, far over a 1 MiB budget. */
    for (unsigned pass = 0; pass < 5; pass++) {
        for (unsigned i = 0; i < NFUN; i++) {
            call_fun(i, 40 + pass);
            if ((i & 127) == 0) smc_check(pass * 1000 + i);
        }
    }
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);

    /* Release the threads blocked across the renewals; each continues on the current JIT. */
    __atomic_store_n(&long_flag, 1, __ATOMIC_RELEASE);
    futex_wake_all(&long_flag);
    __atomic_store_n(&pinned_flag, 1, __ATOMIC_RELEASE);
    futex_wake_all(&pinned_flag);
    for (int i = 0; i < 3; i++) pthread_join(sleepers[i], NULL);
    pthread_join(long_t, NULL);
    pthread_join(pinned_t, NULL);
    for (int i = 0; i < 2; i++) pthread_join(nested_t[i], NULL);

    /* More renewals after the pinned generation could be released. */
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < NFUN; i++) call_fun(i, 90 + pass);

    pid_t child = fork();
    if (child == 0) {
        for (unsigned i = 0; i < 200; i++) call_fun(i * 13, 3);
        _exit(failed ? 1 : 0);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("fork child");

    if (!signals_taken) fail("no signal was taken");
    if (failed) return 1;
    puts("jit_cache_generations: ALL PASS");
    return 0;
}
