// Single-instruction differential driver. Usage:
//   insn_diff [first last]        one hash line per (test, vector)
//   insn_diff detail N V          full input/output state for one case
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "insn_table.h"

struct state {
    uint64_t x[31];   // 0..247
    uint64_t sp;      // 248
    uint64_t nzcv;    // 256
    uint64_t pad;     // 264
    uint64_t v[64];   // 272: q0..q31 (lo, hi)
};
_Static_assert(__builtin_offsetof(struct state, v) == 272, "layout");

typedef void (*test_fn)(struct state*, struct state*);
extern const test_fn g_tests[];
uint64_t g_out, g_hostsp, g_tp;

#define REGION (64 * 1024)
#define MEMBASE 8192            // base register target inside region
#define STACKBASE (56 * 1024)   // sp target inside region
static uint8_t region_low[REGION] __attribute__((aligned(4096)));
static uint8_t* region = region_low;
static uint8_t pattern[REGION] __attribute__((aligned(64)));
static sigjmp_buf env;

static void on_fault(int sig) { siglongjmp(env, sig); }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static void make_input(struct state* in, int t, int vec) {
    static const uint64_t edge[] = {0, 1, ~0ull, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull,
                                    0x8000000000000000ull, 0x7fffffffffffffffull, 0xfffffffffffffffeull,
                                    0x12345678, 0xffff8000, 0x8000, 0x7f, 0x80, 63};
    rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)t << 20) ^ (uint64_t)vec;
    for (int i = 0; i < 31; i++)
        in->x[i] = vec == 0 ? edge[(i + t) % 16] : next();
    for (int i = 0; i < 64; i++)
        in->v[i] = next();
    in->nzcv = vec == 0 ? 0 : vec == 1 ? 0xF0000000ull : (next() & 0xF0000000ull);
    in->sp = (uint64_t)(region + STACKBASE);
    in->pad = 0;
    if (g_info[t].mem) {
        // vec 1/2 put 16- and 8-byte accesses across the page boundary.
        if (g_info[t].rn != 31)
            in->x[g_info[t].rn] = (uint64_t)(region + MEMBASE - (vec == 1 ? 8 : vec == 2 ? 4 : 0));
        if (g_info[t].rm != 31 && g_info[t].rm != g_info[t].rn)
            in->x[g_info[t].rm] = (next() & 15) * (vec + 1);  // small index / data
    }
}

static uint64_t fnv(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 0x100000001b3ull; }
    return h;
}

static uint64_t run(int t, int vec, struct state* in, struct state* out, int* fault) {
    make_input(in, t, vec);
    memset(out, 0, sizeof *out);
    *fault = sigsetjmp(env, 1);
    if (*fault == 0) g_tests[t](in, out);
    else __asm__ volatile("msr tpidr_el0, %0" :: "r"(g_tp));
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, (uint64_t)*fault);
    for (size_t i = 0; i < sizeof *out / 8; i++) h = fnv(h, ((uint64_t*)out)[i]);
    for (size_t i = 0; i < REGION / 8; i++) {
        uint64_t a = ((uint64_t*)region)[i], b = ((uint64_t*)pattern)[i];
        if (a != b) { h = fnv(fnv(h, i), a); ((uint64_t*)region)[i] = b; }
    }
    return h;
}

static void dump(const char* tag, const struct state* s) {
    printf("%s:\n", tag);
    for (int i = 0; i < 31; i++) printf("  x%-2d %016llx%s", i, (unsigned long long)s->x[i], i % 4 == 3 ? "\n" : "");
    printf("  sp  %016llx  nzcv %08llx\n", (unsigned long long)s->sp, (unsigned long long)s->nzcv);
    for (int i = 0; i < 32; i++) printf("  q%-2d %016llx%016llx%s", i, (unsigned long long)s->v[2*i+1],
                                        (unsigned long long)s->v[2*i], i % 2 ? "\n" : "");
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "high")) {
        // Above the 4 GiB window: Bifrost's page-table slow path.
        void* p = mmap((void*)0x71b0000000ull, REGION, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void*)0x71b0000000ull) { perror("mmap high"); return 2; }
        region = p;
        argc--; argv++;
    }
    for (int i = 0; i < REGION; i++) pattern[i] = (uint8_t)(i * 131 + (i >> 8) * 7 + 1);
    memcpy(region, pattern, REGION);
    struct sigaction sa = {0};
    sa.sa_handler = on_fault;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0);
    sigaction(SIGFPE, &sa, 0); sigaction(SIGTRAP, &sa, 0);
    static struct state in, out;
    int fault;
    if (argc == 4 && !strcmp(argv[1], "detail")) {
        int t = atoi(argv[2]), v = atoi(argv[3]);
        printf("test %d vec %d: %08x  %s\n", t, v, g_info[t].word, g_info[t].text);
        make_input(&in, t, v);
        dump("in", &in);
        memcpy(region, pattern, REGION);
        make_input(&in, t, v);
        memset(&out, 0, sizeof out);
        fault = sigsetjmp(env, 1);
        if (fault == 0) g_tests[t](&in, &out);
        else __asm__ volatile("msr tpidr_el0, %0" :: "r"(g_tp));
        printf("fault=%d\n", fault);
        dump("out", &out);
        for (int i = 0; i < REGION / 8; i++)
            if (((uint64_t*)region)[i] != ((uint64_t*)pattern)[i])
                printf("  mem[+%#x] = %016llx (was %016llx)\n", i * 8,
                       (unsigned long long)((uint64_t*)region)[i], (unsigned long long)((uint64_t*)pattern)[i]);
        return 0;
    }
    int first = argc == 3 ? atoi(argv[1]) : 0, last = argc == 3 ? atoi(argv[2]) : NTESTS - 1;
    for (int t = first; t <= last && t < NTESTS; t++)
        for (int v = 0; v < 3; v++)
            printf("%d %d %016llx\n", t, v, (unsigned long long)run(t, v, &in, &out, &fault));
    return 0;
}
