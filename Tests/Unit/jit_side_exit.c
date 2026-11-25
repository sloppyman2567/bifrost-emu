// Loops whose only inner branches are conditional exits (CBZ, CBNZ, TBZ, TBNZ, B.cond) become one block with mid-block exits (SideExit.cpp). Each shape below is
// hand-written so the compiler cannot reshape it, and every result is checked against C: the registers a loop carries must be
// committed at whichever exit is taken, including the first test of the first iteration, and a fault inside the loop must show the
// state of the iterations that finished. The same loops then run on several threads (multithreaded chaining and exit cells).
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

typedef struct node { struct node *next; uint64_t val; } node;
// x0 = list, x1 = key; returns the sum of values up to and including the first node holding key, or -1 for a list without it.
extern int64_t walk(node *, uint64_t);
// x0 = bytes, x1 = out[4]: count, xor, weighted and prefix sums of the bytes before the first zero.
extern void scan(const uint8_t *, uint64_t *);
// x0 = n, x1 = u64 array: sum of the elements before the first zero or n elements, whichever is first; x2 gets the count taken.
extern uint64_t first_zero(uint64_t n, const uint64_t *, uint64_t *taken);
// x0 = u64 array, x2 = n: sum of n elements (a fault inside the loop shows the sum so far).
extern uint64_t sum_n(const uint64_t *, uint64_t, uint64_t);
// Loops that stop on a b.<cond>, tbz or tbnz exit: sum of the elements before the first one for which `element <cond> key` holds (cmp element, key).
extern uint64_t loop_eq(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_ne(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_cs(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_cc(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_mi(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_pl(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_vs(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_vc(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_hi(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_ls(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_ge(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_lt(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_gt(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_le(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_tbz0(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_tbnz0(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_tbz63(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);
extern uint64_t loop_tbnz63(uint64_t n, const uint64_t *, uint64_t key, uint64_t *taken);

__asm__(".text\n.align 4\n.global flag_live\n.type flag_live,%function\nflag_live:\n"
        " cmp x0, x1\n"
        " cbz x2, 2f\n"
        " add x3, x1, #1\n"
        " cset x0, lo\n"
        " add x0, x0, x3\n ret\n"
        "2: cset x0, hi\n ret\n"
        ".size flag_live,.-flag_live\n"
        ".global flag_live_bit\n.type flag_live_bit,%function\nflag_live_bit:\n"
        " cmp x0, x1\n"
        " tbz x2, #0, 2f\n"
        " add x3, x1, #1\n"
        " cset x0, lo\n"
        " add x0, x0, x3\n ret\n"
        "2: cset x0, hi\n ret\n"
        ".size flag_live_bit,.-flag_live_bit\n"
        ".global flag_dead\n.type flag_dead,%function\nflag_dead:\n"
        " cmp x0, x1\n"
        " cbz x2, 2f\n"
        " cmp x1, #7\n"
        " cset x0, hi\n ret\n"
        "2: cmp x0, #3\n cset x0, lo\n ret\n"
        ".size flag_dead,.-flag_dead\n"
        ".global cover_exit\n.type cover_exit,%function\ncover_exit:\n"
        " ldr x3, [x0]\n"
        " cbz x1, 9f\n"
        " ldr x4, [x0]\n"
        " add x3, x3, x4\n"
        " str x3, [x0]\n"
        "9: mov x0, x3\n ret\n"
        ".size cover_exit,.-cover_exit\n");
__asm__(".text\n.align 4\n.global walk\n.type walk,%function\nwalk:\n"
        " mov x3, #0\n"
        "1: cbz x0, 3f\n"
        " ldr x4, [x0, #8]\n"
        " add x3, x3, x4\n"
        " ldr x0, [x0]\n"
        " cmp x4, x1\n"
        " b.ne 1b\n"
        " mov x0, x3\n ret\n"
        "3: mov x0, #-1\n ret\n"
        ".size walk,.-walk\n"
        ".global scan\n.type scan,%function\nscan:\n"
        " mov x2, #0\n mov x3, #0\n mov x4, #1\n mov x5, #0\n"
        "1: ldrb w6, [x0], #1\n"
        " cbz w6, 2f\n"
        " add x2, x2, #1\n"
        " eor x3, x3, x6\n"
        " add x4, x4, x6, lsl #1\n"
        " add x5, x5, x2\n"
        " b 1b\n"
        "2: stp x2, x3, [x1]\n stp x4, x5, [x1, #16]\n ret\n"
        ".size scan,.-scan\n"
        ".global first_zero\n.type first_zero,%function\nfirst_zero:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x3, [x1], #8\n"
        " cbz x3, 2f\n"
        " add x4, x4, x3\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x2]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x2]\n mov x0, x4\n ret\n"
        ".size first_zero,.-first_zero\n"
        ".global sum_n\n.type sum_n,%function\nsum_n:\n"
        " mov x4, #0\n"
        "1: cbz x2, 3f\n"
        " ldr x3, [x0], #8\n"
        " add x4, x4, x3\n"
        " sub x2, x2, #1\n"
        " b 1b\n"
        "3: mov x0, x4\n ret\n"
        ".size sum_n,.-sum_n\n"
        ".global loop_eq\n.type loop_eq,%function\nloop_eq:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.eq 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_eq,.-loop_eq\n"
        ".global loop_ne\n.type loop_ne,%function\nloop_ne:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.ne 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_ne,.-loop_ne\n"
        ".global loop_cs\n.type loop_cs,%function\nloop_cs:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.cs 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_cs,.-loop_cs\n"
        ".global loop_cc\n.type loop_cc,%function\nloop_cc:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.cc 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_cc,.-loop_cc\n"
        ".global loop_mi\n.type loop_mi,%function\nloop_mi:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.mi 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_mi,.-loop_mi\n"
        ".global loop_pl\n.type loop_pl,%function\nloop_pl:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.pl 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_pl,.-loop_pl\n"
        ".global loop_vs\n.type loop_vs,%function\nloop_vs:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.vs 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_vs,.-loop_vs\n"
        ".global loop_vc\n.type loop_vc,%function\nloop_vc:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.vc 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_vc,.-loop_vc\n"
        ".global loop_hi\n.type loop_hi,%function\nloop_hi:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.hi 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_hi,.-loop_hi\n"
        ".global loop_ls\n.type loop_ls,%function\nloop_ls:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.ls 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_ls,.-loop_ls\n"
        ".global loop_ge\n.type loop_ge,%function\nloop_ge:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.ge 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_ge,.-loop_ge\n"
        ".global loop_lt\n.type loop_lt,%function\nloop_lt:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.lt 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_lt,.-loop_lt\n"
        ".global loop_gt\n.type loop_gt,%function\nloop_gt:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.gt 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_gt,.-loop_gt\n"
        ".global loop_le\n.type loop_le,%function\nloop_le:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " cmp x6, x2\n"
        " b.le 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_le,.-loop_le\n"
        ".global loop_tbz0\n.type loop_tbz0,%function\nloop_tbz0:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " tbz x6, #0, 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_tbz0,.-loop_tbz0\n"
        ".global loop_tbnz0\n.type loop_tbnz0,%function\nloop_tbnz0:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " tbnz x6, #0, 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_tbnz0,.-loop_tbnz0\n"
        ".global loop_tbz63\n.type loop_tbz63,%function\nloop_tbz63:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " tbz x6, #63, 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_tbz63,.-loop_tbz63\n"
        ".global loop_tbnz63\n.type loop_tbnz63,%function\nloop_tbnz63:\n"
        " mov x4, #0\n mov x5, x0\n"
        "1: cbz x0, 3f\n"
        " ldr x6, [x1], #8\n"
        " tbnz x6, #63, 2f\n"
        " add x4, x4, x6\n"
        " sub x0, x0, #1\n"
        " b 1b\n"
        "2: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        "3: sub x5, x5, x0\n str x5, [x3]\n mov x0, x4\n ret\n"
        ".size loop_tbnz63,.-loop_tbnz63\n");

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED); } } while (0)


typedef uint64_t (*cond_loop)(uint64_t, const uint64_t *, uint64_t, uint64_t *);
static int cond_holds(int cond, uint64_t a, uint64_t b) {  // the flags of `cmp a, b`
    int64_t d;
    const int v = __builtin_sub_overflow((int64_t)a, (int64_t)b, &d);
    const int n = d < 0, z = a == b, c = a >= b;
    switch (cond) {
        case 0: return z;            case 1: return !z;
        case 2: return c;            case 3: return !c;
        case 4: return n;            case 5: return !n;
        case 6: return v;            case 7: return !v;
        case 8: return c && !z;      case 9: return !c || z;
        case 10: return n == v;      case 11: return n != v;
        case 12: return !z && n == v; default: return z || n != v;
    }
}
static void cond_cases(void) {
    static const cond_loop loops[14] = {loop_eq, loop_ne, loop_cs, loop_cc, loop_mi, loop_pl, loop_vs, loop_vc, loop_hi, loop_ls,
                                        loop_ge, loop_lt, loop_gt, loop_le};
    static const char *names[14] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"};
    const uint64_t interesting[] = {0, 1, 2, 5, 100, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x8000000000000001ull,
                                    0xffffffffffffffffull, 0xfffffffffffffffeull};
    const int ni = sizeof interesting / sizeof *interesting;
    for (int pass = 0; pass < 6; ++pass) {  // the first calls translate each loop, the later ones run it chained
        for (int cond = 0; cond < 14; ++cond) {
            for (int ki = 0; ki < ni; ++ki) {
                const uint64_t key = interesting[ki];
                uint64_t elements[12];
                for (int i = 0; i < 12; ++i) elements[i] = interesting[(i * 7 + ki + pass) % ni];
                for (uint64_t n = 0; n <= 12; n += 3) {
                    uint64_t want = 0, taken = 0;
                    while (taken < n && !cond_holds(cond, elements[taken], key)) want += elements[taken++];
                    uint64_t got_taken = ~0ull, got = loops[cond](n, elements, key, &got_taken);
                    CHECK(got == want && got_taken == taken, "b.%s key=%llx n=%llu: got %llx/%llu want %llx/%llu", names[cond],
                          (unsigned long long)key, (unsigned long long)n, (unsigned long long)got, (unsigned long long)got_taken,
                          (unsigned long long)want, (unsigned long long)taken);
                }
            }
        }
        const struct { cond_loop f; int bit; int set; const char *name; } bits[4] = {{loop_tbz0, 0, 0, "tbz #0"}, {loop_tbnz0, 0, 1, "tbnz #0"},
                                                                                       {loop_tbz63, 63, 0, "tbz #63"}, {loop_tbnz63, 63, 1, "tbnz #63"}};
        for (int b = 0; b < 4; ++b) {
            uint64_t elements[12];
            for (int i = 0; i < 12; ++i) elements[i] = interesting[(i * 3 + pass + b) % ni];
            for (uint64_t n = 0; n <= 12; n += 2) {
                uint64_t want = 0, taken = 0;
                while (taken < n && (int)((elements[taken] >> bits[b].bit) & 1) != bits[b].set) want += elements[taken++];
                uint64_t got_taken = ~0ull, got = bits[b].f(n, elements, 0, &got_taken);
                CHECK(got == want && got_taken == taken, "%s n=%llu: got %llx/%llu want %llx/%llu", bits[b].name, (unsigned long long)n,
                      (unsigned long long)got, (unsigned long long)got_taken, (unsigned long long)want, (unsigned long long)taken);
            }
        }
    }
}

// Flags set before a mid-block exit: read after it on both paths (the exit must commit them), or overwritten first on both (it may skip the store).
extern uint64_t flag_live(uint64_t a, uint64_t b, uint64_t sel);
extern uint64_t flag_dead(uint64_t a, uint64_t b, uint64_t sel);
extern uint64_t flag_live_bit(uint64_t a, uint64_t b, uint64_t sel);
// A second access to the address of the first, after an exit: n == 0 leaves at the exit.
extern uint64_t cover_exit(uint64_t *p, uint64_t n);
static void flag_cases(void) {
    static const uint64_t v[] = {0, 1, 2, 3, 7, 8, 100, 0x8000000000000000ull, 0xffffffffffffffffull};
    for (unsigned i = 0; i < sizeof v / sizeof *v; ++i)
        for (unsigned j = 0; j < sizeof v / sizeof *v; ++j)
            for (uint64_t sel = 0; sel < 2; ++sel) {
                const uint64_t a = v[i], b = v[j];
                CHECK(flag_live(a, b, sel) == (sel ? (uint64_t)(a < b) + b + 1 : (uint64_t)(a > b)), "flag_live(%llx,%llx,%llu)", (unsigned long long)a, (unsigned long long)b, (unsigned long long)sel);
                CHECK(flag_live_bit(a, b, sel) == (sel ? (uint64_t)(a < b) + b + 1 : (uint64_t)(a > b)), "flag_live_bit(%llx,%llx,%llu)", (unsigned long long)a, (unsigned long long)b, (unsigned long long)sel);
                CHECK(flag_dead(a, b, sel) == (sel ? (uint64_t)(b > 7) : (uint64_t)(a < 3)), "flag_dead(%llx,%llx,%llu)", (unsigned long long)a, (unsigned long long)b, (unsigned long long)sel);
            }
    uint64_t cell = 21;
    CHECK(cover_exit(&cell, 0) == 21 && cell == 21, "cover_exit left at the exit: %llu", (unsigned long long)cell);
    CHECK(cover_exit(&cell, 1) == 42 && cell == 42, "cover_exit went on: %llu", (unsigned long long)cell);
}

static void run_all(unsigned rounds) {
    enum { N = 40 };
    node nodes[N];
    for (int i = 0; i < N; ++i) { nodes[i].next = i + 1 < N ? &nodes[i + 1] : NULL; nodes[i].val = 100 + 7 * i; }
    uint8_t text[64];
    uint64_t numbers[N + 8];
    for (unsigned round = 0; round < rounds; ++round) {
        // walk: the key at every position, and a key that is not there
        for (int hit = -1; hit < N; ++hit) {
            uint64_t key = hit < 0 ? 5 : nodes[hit].val;
            int64_t want = -1, sum = 0;
            for (int i = 0; i < N; ++i) { sum += nodes[i].val; if (nodes[i].val == key) { want = sum; break; } }
            int64_t got = walk(nodes, key);
            CHECK(got == want, "walk key %d: got %lld want %lld", hit, (long long)got, (long long)want);
        }
        CHECK(walk(NULL, 1) == -1, "walk of an empty list");
        if (round == 0) cond_cases();
        flag_cases();
        // scan: strings of every length, so the exit is taken at the first test and at the last
        for (int len = 0; len < 40; ++len) {
            for (int i = 0; i < len; ++i) text[i] = (uint8_t)(1 + (i * 37 + round) % 250);
            text[len] = 0;
            uint64_t want[4] = {0, 0, 1, 0}, got[4];
            for (int i = 0; i < len; ++i) { want[0]++; want[1] ^= text[i]; want[2] += (uint64_t)text[i] << 1; want[3] += want[0]; }
            scan(text, got);
            CHECK(!memcmp(got, want, sizeof got), "scan len %d: %llu %llu %llu %llu", len, (unsigned long long)got[0],
                  (unsigned long long)got[1], (unsigned long long)got[2], (unsigned long long)got[3]);
        }
        // first_zero: two exits, a zero at every position and none
        for (int zero = -1; zero < N; ++zero) {
            for (int i = 0; i < N; ++i) numbers[i] = 3 + (uint64_t)i * 5 + round;
            if (zero >= 0) numbers[zero] = 0;
            for (uint64_t n = 0; n <= N; n += 13) {
                uint64_t want = 0, taken = 0;
                while (taken < n && numbers[taken]) want += numbers[taken++];
                uint64_t got_taken = ~0ull, got = first_zero(n, numbers, &got_taken);
                CHECK(got == want && got_taken == taken, "first_zero zero=%d n=%llu: got %llu/%llu want %llu/%llu", zero,
                      (unsigned long long)n, (unsigned long long)got, (unsigned long long)got_taken, (unsigned long long)want,
                      (unsigned long long)taken);
            }
        }
    }
}

// A fault inside the loop: the registers in the signal frame must be those of the iterations that finished.
static sigjmp_buf resume;
static uint64_t fault_x0, fault_x2, fault_x4, fault_addr;
static void on_segv(int sig, siginfo_t *info, void *ctx) {
    (void)sig;
    const ucontext_t *uc = ctx;
    fault_addr = (uint64_t)info->si_addr;
    fault_x0 = uc->uc_mcontext.regs[0];
    fault_x2 = uc->uc_mcontext.regs[2];
    fault_x4 = uc->uc_mcontext.regs[4];
    siglongjmp(resume, 1);
}
static void fault_case(void) {
    const long page = sysconf(_SC_PAGESIZE);
    uint8_t *region = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED || mprotect(region + page, page, PROT_NONE)) { printf("FAIL: mmap\n"); failures++; return; }
    const uint64_t per_page = (uint64_t)page / 8;
    uint64_t *base = (uint64_t *)region;
    for (uint64_t i = 0; i < per_page; ++i) base[i] = i * 3 + 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    for (int attempt = 0; attempt < 3; ++attempt) {  // the loop is translated on the first, chained on the later ones
        fault_addr = 0;
        if (!sigsetjmp(resume, 1)) { sum_n(base, per_page + 50, per_page + 50); CHECK(0, "sum_n did not fault"); continue; }
        uint64_t want_sum = 0;
        for (uint64_t i = 0; i < per_page; ++i) want_sum += base[i];
        CHECK(fault_addr == (uint64_t)(base + per_page), "fault address %llx", (unsigned long long)fault_addr);
        CHECK(fault_x0 == (uint64_t)(base + per_page), "x0 at the fault: %llx", (unsigned long long)fault_x0);
        CHECK(fault_x2 == 50, "x2 at the fault: %llu", (unsigned long long)fault_x2);
        CHECK(fault_x4 == want_sum, "x4 at the fault: %llu want %llu", (unsigned long long)fault_x4, (unsigned long long)want_sum);
    }
    signal(SIGSEGV, SIG_DFL);
    munmap(region, 2 * page);
}

// The exit comes before anything in the loop sets NZCV, so the code it goes to reads the flags the loop's own compare left in the
// iteration before (those of the first compare below when it exits in the first). x5 alternates, so the flags do.
static uint64_t carried_flags(const uint64_t *list) {
    uint64_t nzcv;
    __asm__ volatile("mov x3, %1\n mov x5, #1\n mov w4, #7\n cmp w4, #7\n"
        "1: ldr x2, [x3], #8\n cbz x2, 2f\n eor x5, x5, #8\n cmp x5, #3\n b.ne 1b\n"
        "2: mrs %0, nzcv\n" : "=r"(nzcv) : "r"(list) : "x2", "x3", "x4", "x5", "memory", "cc");
    return nzcv >> 28;
}
static uint64_t carried_flags_bit(const uint64_t *list) {
    uint64_t nzcv;
    __asm__ volatile("mov x3, %1\n mov x5, #1\n mov w4, #7\n cmp w4, #7\n"
        "1: ldr x2, [x3], #8\n tbz x2, #0, 2f\n eor x5, x5, #8\n adds x4, x5, x5, lsl #60\n b.ne 1b\n"
        "2: mrs %0, nzcv\n" : "=r"(nzcv) : "r"(list) : "x2", "x3", "x4", "x5", "memory", "cc");
    return nzcv >> 28;
}
static void carried_flag_cases(void) {
    uint64_t list[40];
    for (int end = 0; end < 32; ++end) {
        for (int i = 0; i < 40; ++i) list[i] = i == end ? 0 : 1;
        // cmp 9, #3 leaves C and cmp 1, #3 leaves N; adds 9, 9 << 60 and 1, 1 << 60 leave N and nothing. Z and C from `cmp w4, #7`.
        const uint64_t want = end == 0 ? 6 : end & 1 ? 2 : 8, want_bit = end == 0 ? 6 : end & 1 ? 8 : 0;
        const uint64_t got = carried_flags(list), got_bit = carried_flags_bit(list);
        CHECK(got == want, "carried_flags end=%d: %llx want %llx", end, (unsigned long long)got, (unsigned long long)want);
        CHECK(got_bit == want_bit, "carried_flags_bit end=%d: %llx want %llx", end, (unsigned long long)got_bit, (unsigned long long)want_bit);
    }
}

static void *worker(void *arg) { (void)arg; run_all(3); return NULL; }

int main(void) {
    run_all(20);
    carried_flag_cases();
    fault_case();
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) pthread_create(&threads[i], NULL, worker, NULL);
    run_all(3);
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    if (failures) { printf("jit_side_exit: %d FAILURES\n", failures); return 1; }
    puts("jit_side_exit: ALL PASS");
    return 0;
}
