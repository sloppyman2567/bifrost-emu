// Reciprocal estimates and fused refinement steps. Fixed edge expectations
// follow Arm pseudocode; corpus hashes were recorded with QEMU (cortex-a53).
// All S/D scalar and vector forms, aliases, upper zeroing, FPSR, FPCR modes.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>

static int checks, failures;
#define CHECK(c, msg) do { ++checks; if (!(c)) { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
#define RUN(ins, dest) __asm__ volatile( \
    "mrs x9, fpcr\n mrs x10, fpsr\n msr fpcr, %[ctl]\n msr fpsr, xzr\n" \
    "ldr q0, [%[a]]\n ldr q1, [%[b]]\n movi v2.16b, #0xa5\n" \
    ins "\n str " dest ", [%[out]]\n mrs %[flags], fpsr\n" \
    "msr fpcr, x9\n msr fpsr, x10\n" \
    : [flags]"=&r"(flags) : [ctl]"r"((uint64_t)control), [a]"r"(a), [b]"r"(b), [out]"r"(out) \
    : "x9", "x10", "v0", "v1", "v2", "memory")

static uint32_t run(unsigned op, unsigned alias, uint32_t control,
                    const uint64_t a[2], const uint64_t b[2], uint64_t out[2]) {
    uint64_t flags = 0;
    switch (op) {
    case 0: // frecps 2s
        if (alias == 1) RUN("frecps v0.2s, v0.2s, v1.2s", "q0");
        else if (alias == 2) RUN("frecps v1.2s, v0.2s, v1.2s", "q1");
        else RUN("frecps v2.2s, v0.2s, v1.2s", "q2");
        break;
    case 1: // frecps 4s
        if (alias == 1) RUN("frecps v0.4s, v0.4s, v1.4s", "q0");
        else if (alias == 2) RUN("frecps v1.4s, v0.4s, v1.4s", "q1");
        else RUN("frecps v2.4s, v0.4s, v1.4s", "q2");
        break;
    case 2: // frecps 2d
        if (alias == 1) RUN("frecps v0.2d, v0.2d, v1.2d", "q0");
        else if (alias == 2) RUN("frecps v1.2d, v0.2d, v1.2d", "q1");
        else RUN("frecps v2.2d, v0.2d, v1.2d", "q2");
        break;
    case 3: // frecps s
        if (alias == 1) RUN("frecps s0, s0, s1", "q0");
        else if (alias == 2) RUN("frecps s1, s0, s1", "q1");
        else RUN("frecps s2, s0, s1", "q2");
        break;
    case 4: // frecps d
        if (alias == 1) RUN("frecps d0, d0, d1", "q0");
        else if (alias == 2) RUN("frecps d1, d0, d1", "q1");
        else RUN("frecps d2, d0, d1", "q2");
        break;
    case 5: // frsqrts 2s
        if (alias == 1) RUN("frsqrts v0.2s, v0.2s, v1.2s", "q0");
        else if (alias == 2) RUN("frsqrts v1.2s, v0.2s, v1.2s", "q1");
        else RUN("frsqrts v2.2s, v0.2s, v1.2s", "q2");
        break;
    case 6: // frsqrts 4s
        if (alias == 1) RUN("frsqrts v0.4s, v0.4s, v1.4s", "q0");
        else if (alias == 2) RUN("frsqrts v1.4s, v0.4s, v1.4s", "q1");
        else RUN("frsqrts v2.4s, v0.4s, v1.4s", "q2");
        break;
    case 7: // frsqrts 2d
        if (alias == 1) RUN("frsqrts v0.2d, v0.2d, v1.2d", "q0");
        else if (alias == 2) RUN("frsqrts v1.2d, v0.2d, v1.2d", "q1");
        else RUN("frsqrts v2.2d, v0.2d, v1.2d", "q2");
        break;
    case 8: // frsqrts s
        if (alias == 1) RUN("frsqrts s0, s0, s1", "q0");
        else if (alias == 2) RUN("frsqrts s1, s0, s1", "q1");
        else RUN("frsqrts s2, s0, s1", "q2");
        break;
    case 9: // frsqrts d
        if (alias == 1) RUN("frsqrts d0, d0, d1", "q0");
        else if (alias == 2) RUN("frsqrts d1, d0, d1", "q1");
        else RUN("frsqrts d2, d0, d1", "q2");
        break;
    case 10: // frecpe 2s
        if (alias == 1) RUN("frecpe v0.2s, v0.2s", "q0");
        else if (alias == 2) RUN("frecpe v1.2s, v0.2s", "q1");
        else RUN("frecpe v2.2s, v0.2s", "q2");
        break;
    case 11: // frecpe 4s
        if (alias == 1) RUN("frecpe v0.4s, v0.4s", "q0");
        else if (alias == 2) RUN("frecpe v1.4s, v0.4s", "q1");
        else RUN("frecpe v2.4s, v0.4s", "q2");
        break;
    case 12: // frecpe 2d
        if (alias == 1) RUN("frecpe v0.2d, v0.2d", "q0");
        else if (alias == 2) RUN("frecpe v1.2d, v0.2d", "q1");
        else RUN("frecpe v2.2d, v0.2d", "q2");
        break;
    case 13: // frecpe s
        if (alias == 1) RUN("frecpe s0, s0", "q0");
        else if (alias == 2) RUN("frecpe s1, s0", "q1");
        else RUN("frecpe s2, s0", "q2");
        break;
    case 14: // frecpe d
        if (alias == 1) RUN("frecpe d0, d0", "q0");
        else if (alias == 2) RUN("frecpe d1, d0", "q1");
        else RUN("frecpe d2, d0", "q2");
        break;
    case 15: // frsqrte 2s
        if (alias == 1) RUN("frsqrte v0.2s, v0.2s", "q0");
        else if (alias == 2) RUN("frsqrte v1.2s, v0.2s", "q1");
        else RUN("frsqrte v2.2s, v0.2s", "q2");
        break;
    case 16: // frsqrte 4s
        if (alias == 1) RUN("frsqrte v0.4s, v0.4s", "q0");
        else if (alias == 2) RUN("frsqrte v1.4s, v0.4s", "q1");
        else RUN("frsqrte v2.4s, v0.4s", "q2");
        break;
    case 17: // frsqrte 2d
        if (alias == 1) RUN("frsqrte v0.2d, v0.2d", "q0");
        else if (alias == 2) RUN("frsqrte v1.2d, v0.2d", "q1");
        else RUN("frsqrte v2.2d, v0.2d", "q2");
        break;
    case 18: // frsqrte s
        if (alias == 1) RUN("frsqrte s0, s0", "q0");
        else if (alias == 2) RUN("frsqrte s1, s0", "q1");
        else RUN("frsqrte s2, s0", "q2");
        break;
    case 19: // frsqrte d
        if (alias == 1) RUN("frsqrte d0, d0", "q0");
        else if (alias == 2) RUN("frsqrte d1, d0", "q1");
        else RUN("frsqrte d2, d0", "q2");
        break;
    }
    return flags;
}
static uint64_t mix(uint64_t hash, uint64_t value) { return (hash ^ value) * 1099511628211ull; }
static uint64_t random_bits(uint64_t* state) { *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17; return *state; }
// Include tiny odd operands, overflow products, cancellation, infinities,
// signed zeros and distinct quiet/signalling NaNs ahead of the random corpus.
static const uint32_t edges32[] = {0,0x80000000,1,0x80000001,0x007fffff,
    0x00800001,0x3f800001,0x3fffffff,0x7f7fffff,0xff7fffff,0x7f800000,
    0xff800000,0x7fc12345,0xff812345,0x3fc00000,0x40000000};
static const uint64_t edges64[] = {0,0x8000000000000000ull,1,0x8000000000000001ull,
    0x000fffffffffffffull,0x0010000000000001ull,0x3ff0000000000001ull,
    0x3fffffffffffffffull,0x7fefffffffffffffull,0xffefffffffffffffull,
    0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8123456789abcull,
    0xfff0123456789abcull,0x3ff8000000000000ull,0x4000000000000000ull};
static uint64_t corpus(uint32_t control) {
    uint64_t state = 0x81a594063f62b87dull, hash = 1469598103934665603ull;
    for (unsigned op=0; op<20; ++op) {
        for (unsigned i=0; i<128; ++i) {
            uint64_t a[2], b[2], out[2];
            for (unsigned lane=0; lane<2; ++lane) {
                if (i<16) {
                    if (op%5==2 || op%5==4) {
                        a[lane]=edges64[(i+lane)%16]; b[lane]=edges64[(15-i+lane)%16];
                    } else {
                        a[lane]=edges32[(i+2*lane)%16] | (uint64_t)edges32[(i+2*lane+1)%16]<<32;
                        b[lane]=edges32[(15-i+2*lane)%16] | (uint64_t)edges32[(16-i+2*lane)%16]<<32;
                    }
                } else { a[lane]=random_bits(&state); b[lane]=random_bits(&state); }
            }
            for (unsigned alias=0; alias<3; ++alias) {
                uint32_t flags=run(op,alias,control,a,b,out);
#ifdef RECIP_DUMP
                printf("%u %u %u %016llx %016llx %x\n", op,i,alias,(unsigned long long)out[0],(unsigned long long)out[1],flags);
#endif
                hash=mix(mix(mix(hash,out[0]),out[1]),flags);
            }
        }
    }
    return hash;
}
static sigjmp_buf jump;
static void ill(int sig) { (void)sig; siglongjmp(jump,1); }
#define UNDEFINED(word) do { if (!sigsetjmp(jump,1)) { __asm__ volatile(".inst " #word ::: "v2"); CHECK(0,"undefined encoding traps"); } else CHECK(1,"undefined encoding traps"); } while (0)
int main(void) {
    uint64_t a[2]={0x7f80000000000000ull,0xff80000080000000ull};
    uint64_t b[2]={0x000000007f800000ull,0x80000000ff800000ull}, out[2];
    CHECK(run(1,0,0,a,b,out)==0 && out[0]==0x4000000040000000ull && out[1]==out[0],"FRECPS infinity*zero = 2, no invalid");
    CHECK(run(6,0,0,a,b,out)==0 && out[0]==0x3fc000003fc00000ull && out[1]==out[0],"FRSQRTS infinity*zero = 1.5, no invalid");
    a[0]=0x3f8000003f800000ull; a[1]=a[0];
    CHECK(run(11,0,0,a,b,out)==0 && out[0]==0x3f7f80003f7f8000ull && out[1]==out[0],"FRECPE architected estimate of 1, not full reciprocal");
    CHECK(run(16,0,0,a,b,out)==0 && out[0]==0x3f7f80003f7f8000ull && out[1]==out[0],"FRSQRTE architected estimate of 1");
    // The exact Unity crash encoding, v3/v16/v18 rather than test registers.
    __asm__ volatile("ldr q16,[%0]\n ldr q18,[%0]\n .inst 0x4e32fe03\n str q3,[%1]" :: "r"(a),"r"(out) : "v3","v16","v18","memory");
    CHECK(out[0]==a[0] && out[1]==a[1],"Unity FRECPS v3.4s,v16.4s,v18.4s");
    // Fused cancellation must retain the product bits lost by a separate multiply.
    a[0]=0x3f800001; b[0]=0x3ffffffe;
    CHECK(run(3,0,0,a,b,out)==0 && out[0]==0x29000000 && out[1]==0,"FRECPS S fused cancellation");
    a[0]=0x3ff0000000000001ull; b[0]=0x3ffffffffffffffeull;
    CHECK(run(4,0,0,a,b,out)==0 && out[0]==0x3980000000000000ull && out[1]==0,"FRECPS D fused cancellation");
    for (unsigned dbl=0; dbl<2; ++dbl) {
        const uint64_t sign=dbl?0x8000000000000000ull:0x80000000;
        const uint64_t one_half=dbl?0x3ff8000000000000ull:0x3fc00000;
        a[0]=1; a[1]=0; b[1]=0;
        for (unsigned neg=0; neg<2; ++neg) {
            b[0]=1 | (neg?sign:0);
            for (unsigned mode=0; mode<4; ++mode) {
                uint64_t want=one_half;
                if (neg && mode==1) ++want;
                if (!neg && (mode==2 || mode==3)) --want;
                CHECK(run(dbl?9:8,0,mode<<22,a,b,out)==16 && out[0]==want && out[1]==0,"FRSQRTS tiny odd inputs and directed rounding");
            }
        }
        a[0]=dbl?0x7fefffffffffffffull:0x7f7fffff;
        b[0]=dbl?0x4000000000000000ull:0x40000000;
        CHECK(run(dbl?9:8,0,0,a,b,out)==16 && out[0]==(a[0]|sign) && out[1]==0,"FRSQRTS halve before rounding avoids intermediate overflow");
    }
    const uint32_t controls[]={0,1u<<22,2u<<22,3u<<22,1u<<24,1u<<25,(1u<<24)|(1u<<25), (3u<<22)|(1u<<24)|(1u<<25)};
    const uint64_t expected[]={0x51a75879b5e7ac19ull,0xe39a5b892ba68055ull,
        0xa355db80c97d7642ull,0xfdd946fcedc250bbull,0xb75bcb1d96fff77bull,
        0x6f0db06909a343a8ull,0x7894c1fee7223c2eull,0xee809a1186405cd8ull};
    for (unsigned i=0; i<8; ++i) {
        uint64_t got=corpus(controls[i]);
        printf("FPCR %08x corpus %016llx\n", controls[i], (unsigned long long)got);
#ifndef RECIP_ORACLE
        CHECK(got==expected[i],"QEMU result bits and FPSR corpus");
#else
        (void)expected;
#endif
    }
    signal(SIGILL,ill);
    UNDEFINED(0x0e60fc02); // FRECPS Q=0,D
    UNDEFINED(0x0ee0fc02); // FRSQRTS Q=0,D
    UNDEFINED(0x0ee1d802); // FRECPE Q=0,D
    UNDEFINED(0x2ee1d802); // FRSQRTE Q=0,D
    UNDEFINED(0x0e403c02); // FRECPS FP16 (not advertised)
    UNDEFINED(0x0ec03c02); // FRSQRTS FP16
    UNDEFINED(0x0ef9d802); // FRECPE FP16
    UNDEFINED(0x2ef9d802); // FRSQRTE FP16
    UNDEFINED(0x5e403c02); // scalar FP16 FRECPS
    UNDEFINED(0x5ec03c02); // scalar FP16 FRSQRTS
    UNDEFINED(0x5ef9d802); // scalar FP16 FRECPE
    UNDEFINED(0x7ef9d802); // scalar FP16 FRSQRTE
    printf("%d/%d checks passed\n", checks-failures,checks);
    return failures ? 1 : 0;
}
