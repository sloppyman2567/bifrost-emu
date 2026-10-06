// Fixed result oracles for the AArch64 repairs carried by bc58e6a.
// Assembly observes full X/V results so C's W narrowing cannot hide errors.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>

static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d\n",__LINE__); } } while (0)

static void loads(void) {
    const int8_t bytes[] = {-128, -1, 0, 127};
    const int16_t halves[] = {-32768, -1, 0, 32767};
    for (unsigned i=0;i<4;i++) {
        uint64_t a,b,c;
        const void* p=bytes+i;
        __asm__ volatile("ldrsb w9,[%3]\n ldursb w10,[%3]\n mov x11,#0\n ldrsb w11,[%3,x11]\n"
                         "mov %0,x9\n mov %1,x10\n mov %2,x11"
                         : "=r"(a),"=r"(b),"=r"(c) : "r"(p) : "x9","x10","x11","memory");
        uint64_t want=(uint32_t)(int32_t)bytes[i];
        CHECK(a==want && b==want && c==want);
        p=halves+i;
        __asm__ volatile("ldrsh w9,[%3]\n ldursh w10,[%3]\n mov x11,#0\n ldrsh w11,[%3,x11]\n"
                         "mov %0,x9\n mov %1,x10\n mov %2,x11"
                         : "=r"(a),"=r"(b),"=r"(c) : "r"(p) : "x9","x10","x11","memory");
        want=(uint32_t)(int32_t)halves[i];
        CHECK(a==want && b==want && c==want);
    }
    int32_t pair[]={INT32_MIN,-1}; uint64_t a,b;
    __asm__ volatile("ldpsw %0,%1,[%2]" : "=&r"(a),"=&r"(b) : "r"(pair) : "memory");
    CHECK(a==0xffffffff80000000ULL && b==UINT64_MAX);
    uint64_t in[]={0x123456789abcdef0ULL,0xfedcba9876543210ULL}, out[2]={0,0};
    __asm__ volatile("ldnp x9,x10,[%2]\n stnp x9,x10,[%3]\n mov %0,x9\n mov %1,x10"
                     : "=r"(a),"=r"(b) : "r"(in),"r"(out) : "x9","x10","memory");
    CHECK(a==in[0] && b==in[1] && !memcmp(in,out,sizeof(in)));
}

static void integer_and_vector(void) {
    uint64_t a,b,sp0,sp1;
    __asm__ volatile("mov %0,sp\n mov x9,#3\n add xzr,x9,x9,lsl #1\n"
                     "sub xzr,x9,x9,lsl #1\n mov %1,sp"
                     : "=&r"(sp0),"=r"(sp1) : : "x9");
    CHECK(sp0==sp1);
    __asm__ volatile("mov x9,#-1\n movk w9,#0x1234\n mov %0,x9\n"
                     "mov x10,#-1\n movk x10,#0,lsl #0\n movk x10,#0,lsl #16\n"
                     "clz w10,w10\n mov %1,x10" : "=r"(a),"=r"(b) : : "x9","x10");
    CHECK(a==0xffff1234ULL && b==32);
    for(unsigned flags=0;flags<16;flags++) {
        __asm__ volatile("msr nzcv,%2\n mov %0,#1\n .inst 0x5400004e\n mov %0,#0\n"
                         "mov %1,#1\n .inst 0x5400004f\n mov %1,#0"
                         : "=&r"(a),"=&r"(b) : "r"((uint64_t)flags<<28) : "cc");
        CHECK(a==1 && b==1);
    }
    uint8_t src[16],clz[16],cls[16];
    for(unsigned i=0;i<16;i++) src[i]=(uint8_t)(i*17);
    __asm__ volatile("ldr q0,[%0]\n clz v1.16b,v0.16b\n cls v2.16b,v0.16b\n"
                     "str q1,[%1]\n str q2,[%2]" : : "r"(src),"r"(clz),"r"(cls) : "v0","v1","v2","memory");
    for(unsigned i=0;i<16;i++) {
        unsigned cz=0,cs=0; uint8_t x=src[i],s=x>>7;
        for(int j=7;j>=0 && !(x&(1u<<j));j--) cz++;
        for(int j=6;j>=0 && ((x>>j)&1)==s;j--) cs++;
        CHECK(clz[i]==cz && cls[i]==cs);
    }
    uint64_t v[2];
    __asm__ volatile("movi v0.16b,#0xff\n fmov d0,#1.0\n str q0,[%0]" : : "r"(v) : "v0","memory");
    CHECK(v[0]==0x3ff0000000000000ULL && v[1]==0);
    __asm__ volatile("movi v0.16b,#0xff\n fmov d0,%1\n str q0,[%0]" : : "r"(v),"r"(0x123456789abcdef0ULL) : "v0","memory");
    CHECK(v[0]==0x123456789abcdef0ULL && v[1]==0);
    __asm__ volatile("movi v0.16b,#0xff\n movi v1.16b,#0xff\n fmov d0,d1\n str q0,[%0]"
                     : : "r"(v) : "v0","v1","memory");
    CHECK(v[0]==UINT64_MAX && v[1]==0);
}

static void conversions(void) {
    static const double values[]={0.0,-1.0,4294967296.0,18446744073709551616.0};
    static const uint64_t xwant[]={0,0,4294967296ULL,UINT64_MAX};
    static const uint64_t wwant[]={0,0,UINT32_MAX,UINT32_MAX};
    for(unsigned i=0;i<4;i++) {
        uint64_t a,b;
        __asm__ volatile("fcvtzu x9,%d2\n fcvtzu w10,%d2\n mov %0,x9\n mov %1,x10"
                         : "=r"(a),"=r"(b) : "w"(values[i]) : "x9","x10");
        CHECK(a==xwant[i] && b==wwant[i]);
    }
    // Values next to rounding boundaries in the unsigned high band.
    static const uint64_t inputs[]={0x8000000000000401ULL,0x8000000000000bffULL,UINT64_MAX};
    static const uint64_t expected[]={0x43e0000000000001ULL,0x43e0000000000001ULL,0x43f0000000000000ULL};
    for(unsigned i=0;i<3;i++) {
        uint64_t bits;
        __asm__ volatile("ucvtf d0,%1\n fmov %0,d0" : "=r"(bits) : "r"(inputs[i]) : "v0");
        CHECK(bits==expected[i]);
    }
}

static void byte_atomic(void) {
    uint8_t bytes[]={0x81,0x22,0x33,0x44};
    register uint8_t* base __asm__("x0")=bytes;
    register uint64_t add __asm__("x9")=3;
    register uint64_t old __asm__("x10");
    __asm__ volatile(".inst 0x3829000a"  // LDADDB W9,W10,[X0]
                     : "=r"(old) : "r"(base),"r"(add) : "memory");
    CHECK(old==0x81 && bytes[0]==0x84 && bytes[1]==0x22 &&
          bytes[2]==0x33 && bytes[3]==0x44);
}

static sigjmp_buf trap;
static volatile sig_atomic_t trapped;
static void on_sigill(int sig) { trapped=sig; siglongjmp(trap,1); }
static void illegal(void) {
    struct sigaction sa={0},saved; sa.sa_handler=on_sigill; sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGILL,&sa,&saved)==0);
    if (!sigsetjmp(trap,1)) __asm__ volatile(".inst 0x00000000");
    CHECK(trapped==SIGILL);
    CHECK(sigaction(SIGILL,&saved,0)==0);
}

int main(int argc, char** argv) {
    loads(); integer_and_vector(); conversions(); byte_atomic();
    // Differential replay does not deliver guest signals; normal JIT and
    // interpreter runs cover the intentional DecodeError-to-SIGILL path.
    if (argc < 2 || strcmp(argv[1], "no-trap")) illegal();
    printf("jit_android_semantics: %d checks %s\n",checks,failures?"FAILED":"passed");
    return failures!=0;
}
