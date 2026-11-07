// jit_neon_struct.c — LD2/LD3/LD4 and ST2/ST3/ST4 multiple structures (de-interleaving vector loads and stores).
//
// FrostJIT runs these natively inside the block: one guard over the whole span, a shuffle network, then the
// post-index base update as plain IR. Every combination is generated as a real instruction word at run time and
// executed, so the same binary also runs under QEMU, which checks the reference model written here:
//   element i of register j sits at byte (i * n + j) * size; a 64-bit arrangement writes the low half and clears
//   the high half; the base moves by the span (immediate form) or by Xm (register form).
// It covers n = 2..4, every element size, both vector widths, register lists at several positions (including
// V28-V31), no writeback / immediate / register post-index (Xm = x2 and x30), SP as the base, a span that ends
// exactly at the end of a mapped page (must succeed) and one byte past it (must fault with PC at the instruction
// and the base unmoved), vector registers kept live across the op in a loop, and a load/store round trip.
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

static int checks, fails;
#define CHECK(ok, ...) do { ++checks; if (!(ok)) { ++fails; if (fails < 40) { printf("NG " __VA_ARGS__); printf("\n"); } } } while (0)

typedef void (*Fn)(uint8_t *base, uint8_t *io, uint64_t step);  // x0 base, x1 io block, x2 step
enum { IO_REGS = 0, IO_BASE = 64, IO_SIZE = 80 };

// ---- encodings -----------------------------------------------------------------------------------------------
static uint32_t enc_struct(int load, int n, int size, int q, int post, int rm, int rn, int rt) {
    static const int opcode[5] = {0, 0, 8, 4, 0};  // LDn/STn: n=2 -> 0b1000, n=3 -> 0b0100, n=4 -> 0b0000
    return (uint32_t)(q << 30) | 0x0C000000u | (uint32_t)(post << 23) | (uint32_t)(load << 22) |
           (uint32_t)(rm << 16) | (uint32_t)(opcode[n] << 12) | (uint32_t)(size << 10) | (uint32_t)(rn << 5) | (uint32_t)rt;
}
static uint32_t enc_str_q(int rt, int rn, int off) { return 0x3D800000u | (uint32_t)(off / 16) << 10 | (uint32_t)rn << 5 | (uint32_t)rt; }
static uint32_t enc_ldr_q(int rt, int rn, int off) { return 0x3DC00000u | (uint32_t)(off / 16) << 10 | (uint32_t)rn << 5 | (uint32_t)rt; }
static uint32_t enc_str_x(int rt, int rn, int off) { return 0xF9000000u | (uint32_t)(off / 8) << 10 | (uint32_t)rn << 5 | (uint32_t)rt; }
static uint32_t enc_movi_a5(int rd) { return 0x4F05E4A0u | (uint32_t)rd; }  // movi vD.16b, #0xa5
#define RET 0xD65F03C0u
#define MOV_X9_X30 0xAA1E03E9u
#define MOV_X30_X2 0xAA0203FEu
#define MOV_X30_X9 0xAA0903FEu
#define MOV_X10_SP 0x910003EAu
#define MOV_SP_X0 0x9100001Fu
#define MOV_X0_SP 0x910003E0u
#define MOV_SP_X10 0x9100015Fu

static uint32_t *code;
static int slot;
typedef struct { int load, n, size, q, post, rm, first, use_sp; } Form;
// Returns the callable code and the address of the structure instruction.
static Fn build(const Form *f, const uint32_t **ins) {
    uint32_t *w = code + slot * 64, *p = w;
    slot++;
    const int rn = f->use_sp ? 31 : 0, vector = f->q ? 16 : 8;
    (void)vector;
    if (f->rm == 30) { *p++ = MOV_X9_X30; *p++ = MOV_X30_X2; }
    if (f->load) for (int j = 0; j < f->n; j++) *p++ = enc_movi_a5(f->first + j);
    else for (int j = 0; j < f->n; j++) *p++ = enc_ldr_q(f->first + j, 1, 16 * j);
    if (f->use_sp) { *p++ = MOV_X10_SP; *p++ = MOV_SP_X0; }
    *ins = p;
    *p++ = enc_struct(f->load, f->n, f->size, f->q, f->post, f->post ? f->rm : 0, rn, f->first);
    if (f->use_sp) { *p++ = MOV_X0_SP; *p++ = MOV_SP_X10; }
    if (f->load) for (int j = 0; j < f->n; j++) *p++ = enc_str_q(f->first + j, 1, 16 * j);
    *p++ = enc_str_x(0, 1, IO_BASE);
    if (f->rm == 30) *p++ = MOV_X30_X9;
    *p++ = RET;
    __builtin___clear_cache((char *)w, (char *)p);
    return (Fn)w;
}

// ---- reference model -----------------------------------------------------------------------------------------
static void model_load(int n, int e, int q, const uint8_t *mem, uint8_t out[4][16]) {
    memset(out, 0, 64);
    for (int lane = 0; lane < (q ? 16 : 8) / e; lane++)
        for (int j = 0; j < n; j++) memcpy(&out[j][lane * e], mem + (lane * n + j) * e, e);
}
static void model_store(int n, int e, int q, const uint8_t in[4][16], uint8_t *mem) {
    for (int lane = 0; lane < (q ? 16 : 8) / e; lane++)
        for (int j = 0; j < n; j++) memcpy(mem + (lane * n + j) * e, &in[j][lane * e], e);
}
static const char *kind(const Form *f) {
    static char s[96];
    snprintf(s, sizeof s, "%s%d size %d q %d v%d %s", f->load ? "ld" : "st", f->n, 1 << f->size, f->q, f->first,
             !f->post ? "plain" : f->rm == 31 ? "imm" : f->rm == 30 ? "x30" : "reg");
    return s;
}

static void run_form(const Form *f, int seed) {
    const int e = 1 << f->size, span = f->n * (f->q ? 16 : 8);
    const uint64_t step = 40;
    uint8_t memory[256] __attribute__((aligned(16))), expect_memory[256];
    uint8_t io[IO_SIZE] = {0}, regs[4][16];
    const uint32_t *ins;
    const Fn fn = build(f, &ins);
    (void)ins;
    for (int i = 0; i < 256; i++) memory[i] = expect_memory[i] = (uint8_t)(i * 37 + seed * 11 + 5);
    uint8_t *base = memory + 64;
    uint64_t advance = !f->post ? 0 : f->rm == 31 ? (uint64_t)span : step;
    if (f->load) {
        model_load(f->n, e, f->q, base, regs);
        fn(base, io, step);
        for (int j = 0; j < f->n; j++)
            CHECK(!memcmp(io + 16 * j, regs[j], 16), "%s seed %d: v%d wrong", kind(f), seed, f->first + j);
    } else {
        for (int j = 0; j < f->n; j++) {
            for (int i = 0; i < 16; i++) regs[j][i] = (uint8_t)(j * 53 + i * 7 + seed * 3 + 1);
            if (!f->q) memset(&regs[j][8], 0xEE, 8);  // the high half of a 64-bit arrangement must be ignored
            memcpy(io + 16 * j, regs[j], 16);
        }
        model_store(f->n, e, f->q, regs, expect_memory + 64);
        fn(base, io, step);
        CHECK(!memcmp(memory, expect_memory, 256), "%s seed %d: memory wrong", kind(f), seed);
    }
    uint64_t moved;
    memcpy(&moved, io + IO_BASE, 8);
    CHECK(moved == (uint64_t)base + advance, "%s seed %d: base moved by %lld want %llu", kind(f), seed,
          (long long)(moved - (uint64_t)base), (unsigned long long)advance);
}

// ---- faults and page edges -----------------------------------------------------------------------------------
static sigjmp_buf jump;
static volatile uint64_t fault_pc, fault_x0;
static volatile int faulted;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    fault_pc = uc->uc_mcontext.pc;
    fault_x0 = uc->uc_mcontext.regs[0];
    faulted = 1;
    siglongjmp(jump, 1);
}
static void edge_cases(void) {
    uint8_t *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED || mprotect(pages + 4096, 4096, PROT_NONE)) { CHECK(0, "cannot map the edge pages"); return; }
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    for (int load = 0; load < 2; load++)
        for (int n = 2; n <= 4; n++)
            for (int size = 0; size < 4; size++)
                for (int q = 0; q < 2; q++) {
                    if (size == 3 && !q) continue;
                    Form f = {load, n, size, q, 1, 31, 4, 0};
                    const int span = n * (q ? 16 : 8);
                    for (int cross = 0; cross < 2; cross++) {
                        uint8_t *base = pages + 4096 - span + cross;  // cross = 1: the last byte lies on the PROT_NONE page
                        uint8_t io[IO_SIZE] = {0};
                        for (int i = 0; i < 64; i++) io[i] = (uint8_t)(i * 5 + 1);
                        const uint32_t *ins;
                        const Fn fn = build(&f, &ins);
                        faulted = 0;
                        if (sigsetjmp(jump, 1) == 0) fn(base, io, 0);
                        if (!cross) {
                            CHECK(!faulted, "%s: a span ending exactly at the page end faulted", kind(&f));
                        } else {
                            CHECK(faulted, "%s: no fault one byte past the page", kind(&f));
                            CHECK(faulted && fault_pc == (uint64_t)ins, "%s: fault pc %llx want %llx", kind(&f),
                                  (unsigned long long)fault_pc, (unsigned long long)(uint64_t)ins);
                            CHECK(faulted && fault_x0 == (uint64_t)base, "%s: base moved to %llx on a fault", kind(&f),
                                  (unsigned long long)fault_x0);
                        }
                    }
                }
}

// ---- vectors live across the op ------------------------------------------------------------------------------
// Vector state dirty before the op, used after it, in a loop; plus scalar FP around it.
static void loop_cases(void) {
    float in[64 * 4], out[64 * 4], want[64 * 4];
    for (int i = 0; i < 64 * 4; i++) in[i] = (float)(i % 13) - 6.25f;
    // out = (field0 + field1 * 2 + field2 + field3) for each of 64 structures, kept in v0, then stored with ST4
    // as four copies. A dirty accumulator (v0) and a constant (v1) stay live across both structure ops.
    float acc[4] = {0, 0, 0, 0};
    for (int i = 0; i < 64; i += 4) {
        for (int l = 0; l < 4; l++) {
            const float *s = &in[(i + l) * 4];
            const float x = s[0] + s[1] * 2.0f + s[2] + s[3];
            acc[l] += x;
            for (int j = 0; j < 4; j++) want[(i + l) * 4 + j] = acc[l];
        }
    }
    const float *ip = in;
    float *op = out;
    __asm__ volatile(
        "movi v0.4s, #0\n fmov v1.4s, #2.0\n mov x9, %[n]\n"
        "1: ld4 {v4.4s-v7.4s}, [%[ip]], #64\n"
        "fadd v2.4s, v4.4s, v6.4s\n fmla v2.4s, v5.4s, v1.4s\n fadd v2.4s, v2.4s, v7.4s\n"
        "fadd v0.4s, v0.4s, v2.4s\n mov v8.16b, v0.16b\n mov v9.16b, v0.16b\n mov v10.16b, v0.16b\n mov v11.16b, v0.16b\n"
        "st4 {v8.4s-v11.4s}, [%[op]], #64\n subs x9, x9, #1\n b.ne 1b\n"
        : [ip] "+r"(ip), [op] "+r"(op)
        : [n] "r"((long)16)
        : "memory", "cc", "x9", "v0", "v1", "v2", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11");
    CHECK(!memcmp(out, want, sizeof want), "ld4/st4 loop with live vectors wrong (first %g want %g)", out[0], want[0]);
    // Scalar FP before and after an LD2 in one block.
    float pair[8] = {1, 2, 3, 4, 5, 6, 7, 8}, r[2];
    float s0 = 1.5f, s1 = 2.5f, a, b;
    __asm__ volatile(
        "fmov s0, %w[x]\n fmov s1, %w[y]\n fadd s2, s0, s1\n ld2 {v4.4s, v5.4s}, [%[p]]\n fmul s3, s2, s4\n fadd s6, s3, s5\n"
        "str s3, [%[r]]\n str s6, [%[r], #4]\n"
        : : [x] "r"(*(uint32_t *)&s0), [y] "r"(*(uint32_t *)&s1), [p] "r"(pair), [r] "r"(r)
        : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6");
    a = (1.5f + 2.5f) * 1.0f; b = a + 2.0f;
    CHECK(r[0] == a && r[1] == b, "scalar FP around ld2: %g %g want %g %g", r[0], r[1], a, b);
    // Load then store the same layout: memory is unchanged.
    uint8_t src[64], dst[64];
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)(i * 29 + 3);
    memset(dst, 0, 64);
    __asm__ volatile("ld3 {v16.16b-v18.16b}, [%[s]]\n st3 {v16.16b-v18.16b}, [%[d]]\n"
        : : [s] "r"(src), [d] "r"(dst) : "memory", "v16", "v17", "v18");
    CHECK(!memcmp(src, dst, 48) && dst[48] == 0, "ld3/st3 round trip");
}

// Twelve live accumulators (v16-v27) and the four structure registers: the allocator is under register pressure
// around an LD4 and an ST4 in the same loop, so pinned vectors share the host with the ops' scratch registers.
static void pressure_case(void) {
    enum { ITER = 6 };
    float in[ITER * 16], out[ITER * 16], want[ITER * 16], final[12 * 4], want_final[12 * 4];
    for (int i = 0; i < ITER * 16; i++) in[i] = (float)(i % 11) - 4.5f;
    float acc[12][4] = {{0}};
    for (int t = 0; t < ITER; t++) {
        for (int k = 0; k < 12; k++)
            for (int l = 0; l < 4; l++) acc[k][l] += in[(t * 4 + l) * 4 + k % 4];
        for (int l = 0; l < 4; l++)
            for (int j = 0; j < 4; j++) want[t * 16 + l * 4 + j] = acc[j][l];
    }
    for (int k = 0; k < 12; k++) memcpy(&want_final[k * 4], acc[k], 16);
    const float *ip = in;
    float *op = out;
    __asm__ volatile(
        "movi v16.4s, #0\n movi v17.4s, #0\n movi v18.4s, #0\n movi v19.4s, #0\n movi v20.4s, #0\n movi v21.4s, #0\n"
        "movi v22.4s, #0\n movi v23.4s, #0\n movi v24.4s, #0\n movi v25.4s, #0\n movi v26.4s, #0\n movi v27.4s, #0\n"
        "mov x9, %[n]\n"
        "1: ld4 {v4.4s-v7.4s}, [%[ip]], #64\n"
        "fadd v16.4s, v16.4s, v4.4s\n fadd v17.4s, v17.4s, v5.4s\n fadd v18.4s, v18.4s, v6.4s\n fadd v19.4s, v19.4s, v7.4s\n"
        "fadd v20.4s, v20.4s, v4.4s\n fadd v21.4s, v21.4s, v5.4s\n fadd v22.4s, v22.4s, v6.4s\n fadd v23.4s, v23.4s, v7.4s\n"
        "fadd v24.4s, v24.4s, v4.4s\n fadd v25.4s, v25.4s, v5.4s\n fadd v26.4s, v26.4s, v6.4s\n fadd v27.4s, v27.4s, v7.4s\n"
        "st4 {v16.4s-v19.4s}, [%[op]], #64\n subs x9, x9, #1\n b.ne 1b\n"
        "str q16, [%[f]]\n str q17, [%[f], #16]\n str q18, [%[f], #32]\n str q19, [%[f], #48]\n"
        "str q20, [%[f], #64]\n str q21, [%[f], #80]\n str q22, [%[f], #96]\n str q23, [%[f], #112]\n"
        "str q24, [%[f], #128]\n str q25, [%[f], #144]\n str q26, [%[f], #160]\n str q27, [%[f], #176]\n"
        : [ip] "+r"(ip), [op] "+r"(op)
        : [n] "r"((long)ITER), [f] "r"(final)
        : "memory", "cc", "x9", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24",
          "v25", "v26", "v27");
    CHECK(!memcmp(out, want, sizeof want), "ld4/st4 under register pressure: stored rows wrong");
    CHECK(!memcmp(final, want_final, sizeof want_final), "ld4/st4 under register pressure: live accumulators wrong");
}

// Xm in a post-index form is any of X0-X30: X30 is an ordinary register, not "#0". Covers the forms around
// LD2-LD4: single-register LD1, single lanes and the replicate load.
static void x30_cases(void) {
    static uint8_t buf[128] __attribute__((aligned(16)));
    uint64_t b;
    b = (uint64_t)buf;
    __asm__ volatile("mov x30, #40\n ld1 {v16.s}[1], [%0], x30\n" : "+r"(b) : : "x30", "v16", "memory");
    CHECK(b - (uint64_t)buf == 40, "ld1 lane post-index by x30 moved %lld", (long long)(b - (uint64_t)buf));
    b = (uint64_t)buf;
    __asm__ volatile("mov x30, #40\n st1 {v16.s}[2], [%0], x30\n" : "+r"(b) : : "x30", "memory");
    CHECK(b - (uint64_t)buf == 40, "st1 lane post-index by x30 moved %lld", (long long)(b - (uint64_t)buf));
    b = (uint64_t)buf;
    __asm__ volatile("mov x30, #40\n ld1r {v16.4s}, [%0], x30\n" : "+r"(b) : : "x30", "v16", "memory");
    CHECK(b - (uint64_t)buf == 40, "ld1r post-index by x30 moved %lld", (long long)(b - (uint64_t)buf));
    b = (uint64_t)buf;
    __asm__ volatile("mov x30, #40\n ld1 {v16.16b}, [%0], x30\n" : "+r"(b) : : "x30", "v16", "memory");
    CHECK(b - (uint64_t)buf == 40, "ld1 post-index by x30 moved %lld", (long long)(b - (uint64_t)buf));
    b = (uint64_t)buf;
    __asm__ volatile("mov x30, #40\n st1 {v16.16b, v17.16b}, [%0], x30\n" : "+r"(b) : : "x30", "memory");
    CHECK(b - (uint64_t)buf == 40, "st1 two registers post-index by x30 moved %lld", (long long)(b - (uint64_t)buf));
}

int main(int argc, char **argv) {
    // "no-fault": skip the section that faults on purpose, which BIFROST_JIT_VERIFY replay cannot follow.
    const int faults = !(argc > 1 && !strcmp(argv[1], "no-fault"));
    code = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { printf("jit_neon_struct: cannot map code\n"); return 1; }
    static const int firsts[3] = {1, 16, 28};
    for (int load = 0; load < 2; load++)
        for (int n = 2; n <= 4; n++)
            for (int size = 0; size < 4; size++)
                for (int q = 0; q < 2; q++) {
                    if (size == 3 && !q) continue;  // .1D is not a valid arrangement for LD2-LD4
                    for (int fi = 0; fi < 3; fi++)
                        for (int mode = 0; mode < 4; mode++) {
                            Form f = {load, n, size, q, mode != 0, mode == 1 ? 31 : mode == 2 ? 2 : 30, firsts[fi], 0};
                            if (!f.post) f.rm = 0;
                            if (slot > 900) slot = 0;  // reuse slots; each is rewritten before it runs
                            run_form(&f, fi * 4 + mode + 1);
                        }
                }
    Form sp = {1, 4, 0, 1, 1, 31, 20, 1};  // v20-v23: v8-v15 are callee-saved
    run_form(&sp, 3);
    sp.load = 0; run_form(&sp, 4);
    sp.n = 2; sp.size = 2; run_form(&sp, 5);
    if (faults) edge_cases();
    loop_cases();
    pressure_case();
    x30_cases();
    if (fails) { printf("jit_neon_struct: FAILED (%d of %d)\n", fails, checks); return 1; }
    printf("jit_neon_struct: ALL PASS (%d checks)\n", checks);
    return 0;
}
