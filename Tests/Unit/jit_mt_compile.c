/* Many threads translating at once: each runs its own generated functions once, all of them run a shared set, some functions
 * fault on purpose, one thread keeps rewriting a function while the others call it, and one forks in the middle. A model in C
 * gives every result; the faults are counted exactly, because a lost fault record ends the process instead.
 *     jit_mt_compile.elf [threads] [functions per thread] [forks] */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

enum { WORDS = 16, SHARED = 400, MARK = 0x55 };
typedef uint64_t (*Fn)(uint64_t, uint64_t *);
static int threads = 8, own = 1200, forks = 3;
static uint32_t *code, *changing;
static uint64_t *bad;
static pthread_barrier_t start;
static volatile int failed, stop;
static _Thread_local volatile unsigned faults;

static void generate(uint32_t *p, uint32_t i) {
    const uint32_t body[] = {
        0xF9400023u,                            /* ldr x3,[x1] */
        0x91000000u | ((i & 0xfff) << 10),      /* add x0,x0,#i */
        0xD2800002u | ((i & 0xffff) << 5),      /* mov x2,#i */
        0xCA020000u,                            /* eor x0,x0,x2 */
        0xF100001Fu | ((i & 0xff) << 10),       /* cmp x0,#i */
        0x54000060u,                            /* b.eq +12 */
        0x8B020C00u,                            /* add x0,x0,x2,lsl #3 */
        0x14000002u,                            /* b +8 */
        0xCB020000u,                            /* sub x0,x0,x2 */
        0x8B030000u,                            /* add x0,x0,x3 */
        0xF9000020u,                            /* str x0,[x1] */
        0xD65F03C0u                             /* ret */
    };
    memcpy(p, body, sizeof body);
}
static uint64_t model(uint32_t i, uint64_t x, uint64_t *mem) {
    const uint64_t loaded = mem ? *mem : MARK, k = i & 0xffff;
    x = (x + (i & 0xfff)) ^ k;
    x = x == (i & 0xff) ? x - k : x + (k << 3);
    x += loaded;
    if (mem) *mem = x;
    return x;
}
static void on_fault(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (info->si_addr != (void *)bad) { signal(sig, SIG_DFL); return; }
    if (*(uint32_t *)uc->uc_mcontext.pc == 0xF9400023u) uc->uc_mcontext.regs[3] = MARK;
    uc->uc_mcontext.pc += 4;
    ++faults;
}
/* Runs functions first..first+count and compares with the model. Every seventh gets the unmapped page. */
static int run(uint32_t first, int count, uint64_t seed) {
    uint64_t mem[1] = {seed}, want_mem[1] = {seed}, x = seed, want = seed;
    const unsigned faults_before = faults;
    unsigned want_faults = 0;
    for (int n = 0; n < count; ++n) {
        const uint32_t i = first + (uint32_t)n;
        const int faulting = i % 7 == 0;
        x = ((Fn)(code + (size_t)i * WORDS))(x, faulting ? bad : mem);
        want = model(i, want, faulting ? 0 : want_mem);
        want_faults += faulting ? 2 : 0;
        if (x != want || mem[0] != want_mem[0] || faults - faults_before != want_faults) {
            printf("FAIL function %u: got %llx want %llx, memory %llx want %llx, faults %u want %u\n", i, (unsigned long long)x,
                   (unsigned long long)want, (unsigned long long)mem[0], (unsigned long long)want_mem[0], faults - faults_before, want_faults);
            failed = 1;
            return 0;
        }
    }
    return 1;
}
/* A function whose immediate alternates between two values: any call sees one of them. */
static void write_changing(unsigned value) {
    changing[0] = 0xD2800000u | (value << 5);   /* mov x0,#value */
    __builtin___clear_cache((char *)changing, (char *)(changing + 1));
}
static void *change(void *unused) {
    (void)unused;
    for (unsigned n = 0; !stop; ++n) { write_changing(n & 1 ? 11 : 22); usleep(200); }
    return 0;
}
static void *work(void *arg) {
    const uint32_t id = (uint32_t)(uintptr_t)arg;
    pthread_barrier_wait(&start);
    for (int n = 0; n < own && !failed; n += 100) {
        const int count = own - n < 100 ? own - n : 100;
        if (!run(SHARED + id * (uint32_t)own + (uint32_t)n, count, 7 + id)) break;
        run((uint32_t)(n % SHARED), 100, 99 + id);
        const uint64_t v = ((uint64_t (*)(void))changing)();
        if (v != 11 && v != 22) { printf("FAIL rewritten function returned %llu\n", (unsigned long long)v); failed = 1; }
        if (id == 0 && n / 100 < forks) {
            const pid_t child = fork();
            if (child == 0) _exit(run(SHARED + (uint32_t)(threads - 1) * (uint32_t)own, own, 5) ? 0 : 1);
            int status = 0;
            if (child < 0 || waitpid(child, &status, 0) != child || status != 0) { printf("FAIL fork: status %x\n", status); failed = 1; }
        }
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1) threads = atoi(argv[1]);
    if (argc > 2) own = atoi(argv[2]);
    if (argc > 3) forks = atoi(argv[3]);
    const size_t functions = SHARED + (size_t)threads * own, bytes = functions * WORDS * 4;
    code = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    changing = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bad = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (threads < 1 || threads > 64 || code == MAP_FAILED || changing == MAP_FAILED || bad == MAP_FAILED) return 2;
    for (size_t i = 0; i < functions; ++i) generate(code + i * WORDS, (uint32_t)i);
    mprotect(code, bytes, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)code, (char *)code + bytes);
    changing[1] = 0xD65F03C0u;
    write_changing(11);
    struct sigaction action = {0};
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &action, 0);
    pthread_barrier_init(&start, 0, (unsigned)threads);
    pthread_t pool[64], changer;
    pthread_create(&changer, 0, change, 0);
    for (int t = 0; t < threads; ++t) pthread_create(&pool[t], 0, work, (void *)(uintptr_t)t);
    for (int t = 0; t < threads; ++t) pthread_join(pool[t], 0);
    stop = 1;
    pthread_join(changer, 0);
    puts(failed ? "jit_mt_compile: FAIL" : "jit_mt_compile: ALL PASS");
    return failed;
}
