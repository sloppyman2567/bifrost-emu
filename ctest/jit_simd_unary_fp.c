/* Doom 3's vector FSQRT must not alias FNEG (encoding bit 16).
 * Fixed expected results check width, aliasing, signed zero and upper clearing.
 * The table check reproduces the vectorized idMath::Init seed calculation.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
static int failures, checks;
static void check(const char *name,const uint64_t got[2],const uint64_t want[2]) {
    ++checks;
    if(got[0]!=want[0] || got[1]!=want[1]) {
        printf("FAIL %s got=%016llx:%016llx want=%016llx:%016llx\n",name,
            (unsigned long long)got[1],(unsigned long long)got[0],
            (unsigned long long)want[1],(unsigned long long)want[0]);
        ++failures;
    }
}
#define UNARY(op,form,input,want) do { \
    uint64_t got[2]; \
    __asm__ volatile("b 1f\n 1: ldr q26,[%0]\n movi v27.16b,#0xa5\n " op " v27." form ",v26." form "\n str q27,[%1]\n b 2f\n 2:" \
        ::"r"(input),"r"(got):"v26","v27","memory"); \
    check(op " " form,got,want); \
    __asm__ volatile("b 1f\n 1: ldr q26,[%0]\n " op " v26." form ",v26." form "\n str q26,[%1]\n b 2f\n 2:" \
        ::"r"(input),"r"(got):"v26","memory"); \
    check(op " alias " form,got,want); \
} while(0)
static void high_host_regs(void) {
    float input[4]={2,4,8,16}, output[4];
    const float want[4]={2,6,14,30};
    // Thirteen live vector registers force operands into host XMM8-15.
    // Read the newly-written cached destination at every subsequent op.
    __asm__ volatile("b 1f\n 1:\n"
        "movi v0.4s,#0x40,lsl #24\n movi v1.4s,#0x40,lsl #24\n"
        "movi v2.4s,#0x40,lsl #24\n movi v3.4s,#0x40,lsl #24\n"
        "movi v4.4s,#0x40,lsl #24\n movi v5.4s,#0x40,lsl #24\n"
        "movi v6.4s,#0x40,lsl #24\n movi v7.4s,#0x40,lsl #24\n"
        "movi v8.4s,#0x40,lsl #24\n movi v9.4s,#0x40,lsl #24\n"
        "movi v10.4s,#0x40,lsl #24\n movi v11.4s,#0x40,lsl #24\n"
        "ldr q12,[%0]\n fmul v12.4s,v12.4s,v11.s[3]\n"
        "fneg v12.4s,v12.4s\n fabs v12.4s,v12.4s\n"
        "fabd v12.4s,v12.4s,v10.4s\n str q12,[%1]\n b 2f\n 2:"
        ::"r"(input),"r"(output):"v0","v1","v2","v3","v4","v5",
          "v6","v7","v8","v9","v10","v11","v12","memory");
    uint64_t got_bits[2],want_bits[2];
    memcpy(got_bits,output,16);memcpy(want_bits,want,16);
    check("cached high host regs",got_bits,want_bits);
}
#define ABS_DIFF(form, type, lanes, rd, rn, rm) do { \
    type a[4]={2,4,8,16}, b[4]={-1,-2,-4,-8}, want[4]={0}; \
    uint64_t got[2]; \
    for(int i=0;i<lanes;i++) want[i]=rn==rm ? 0 : a[i]-b[i]; \
    __asm__ volatile("b 1f\n 1: ldr q26,[%0]\n ldr q31,[%1]\n" \
        "fabd v" #rd "." form ",v" #rn "." form ",v" #rm "." form "\n" \
        "str q" #rd ",[%2]\n b 2f\n 2:" \
        ::"r"(a),"r"(b),"r"(got):"v26","v27","v31","memory"); \
    uint64_t expected[2]; memcpy(expected,want,16); \
    check("fabd " form " dst=" #rd " lhs=" #rn " rhs=" #rm,got,expected); \
} while(0)
static void *worker(void *arg) { return arg; }
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--mt") == 0) {
        pthread_t thread;
        if (pthread_create(&thread,NULL,worker,NULL) || pthread_join(thread,NULL)) return 2;
    }
    high_host_regs();
    const uint64_t single[2]={0x408000003f800000ULL,0x8000000041800000ULL};
    const uint64_t ss[2]={0x400000003f800000ULL,0x8000000040800000ULL};
    const uint64_t sn[2]={0xc0800000bf800000ULL,0x00000000c1800000ULL};
    const uint64_t sa[2]={0x408000003f800000ULL,0x0000000041800000ULL};
    const uint64_t ss2[2]={ss[0],0},sn2[2]={sn[0],0},sa2[2]={sa[0],0};
    const uint64_t dbl[2]={0x4030000000000000ULL,0x8000000000000000ULL};
    const uint64_t ds[2]={0x4010000000000000ULL,0x8000000000000000ULL};
    const uint64_t dn[2]={0xc030000000000000ULL,0};
    const uint64_t da[2]={0x4030000000000000ULL,0};
    UNARY("fsqrt","2s",single,ss2); UNARY("fsqrt","4s",single,ss);
    UNARY("fneg","2s",single,sn2); UNARY("fneg","4s",single,sn);
    UNARY("fabs","2s",single,sa2); UNARY("fabs","4s",single,sa);
    UNARY("fsqrt","2d",dbl,ds); UNARY("fneg","2d",dbl,dn); UNARY("fabs","2d",dbl,da);
    ABS_DIFF("4s",float,4,27,26,31); ABS_DIFF("4s",float,4,26,26,31);
    ABS_DIFF("4s",float,4,31,26,31); ABS_DIFF("4s",float,4,26,26,26);
    ABS_DIFF("2s",float,2,27,26,31); ABS_DIFF("2s",float,2,31,26,31);
    ABS_DIFF("2d",double,2,27,26,31); ABS_DIFF("2d",double,2,26,26,31);
    ABS_DIFF("2d",double,2,31,26,31); ABS_DIFF("2d",double,2,26,26,26);
    for(int i=0;i<512;i+=4) {
        uint32_t index[4]={(uint32_t)i,(uint32_t)i+1,(uint32_t)i+2,(uint32_t)i+3},got[4];
        __asm__ volatile("ldr q31,[%0]\n shl v26.4s,v31.4s,#15\n"
            "orr v26.4s,#0x3f,lsl #24\n fsqrt v26.4s,v26.4s\n"
            "fmov v30.4s,#1.0\n fdiv v26.4s,v30.4s,v26.4s\n"
            "movi v29.4s,#0x20,lsl #8\n add v26.4s,v26.4s,v29.4s\n str q26,[%1]"
            ::"r"(index),"r"(got):"v26","v29","v30","v31","memory");
        for(int j=0;j<4;j++) {
            uint32_t bits=0x3f000000u|((uint32_t)(i+j)<<15);float x,y;
            memcpy(&x,&bits,4); y=1.f/sqrtf(x);memcpy(&bits,&y,4);
            uint32_t want=(bits+0x2000u)&0x7f8000u;
            ++checks;
            if((got[j]&0x7f8000u)!=want) {printf("FAIL seed %d got=%x want=%x\n",i+j,got[j]&0x7f8000u,want);++failures;}
        }
    }
    if(failures)return 1;
    printf("simd_unary_fp: ALL PASS (%d checks)\n",checks);return 0;
}
