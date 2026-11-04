/* Indexed FP multiply and fused multiply-add/subtract: high scalar lanes,
 * high registers, aliases and Q=0. FMLA/FMLS use values whose product only
 * survives with a single rounding (1+2^-12)(1-2^-12) - 1. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
static int checks, failures;
#define TEST(form,scalar,lane,rd,rn,rm,type,n) do { \
 type a[4]={2,4,8,16}, b[4]={-1,2,-4,8}, got[4]={0}, want[4]={0}; \
 if(rn==rm) memcpy(b,a,sizeof(a)); \
 for(int i=0;i<n;i++) want[i]=a[i]*b[lane]; \
 __asm__ volatile("b 1f\n 1: ldr q" #rn ",[%0]\n ldr q" #rm ",[%1]\n fmul v" #rd "." form ",v" #rn "." form ",v" #rm "." scalar "[" #lane "]\n str q" #rd ",[%2]\n b 2f\n 2:" \
 ::"r"(a),"r"(b),"r"(got):"v26","v27","v31","memory"); \
 ++checks; if(memcmp(got,want,16)) {printf("FAIL %s lane %d dst %d src %d scalar %d\n",form,lane,rd,rn,rm); ++failures;} \
}while(0)
#define TESTF(op,sgn,form,scalar,lane,rd,rn,rm,type,n,fma) do { \
 type a[4]={2,4,(type)1+(type)1/4096,16}, b[4]={-1,(type)1-(type)1/4096,-4,8}, c[4]={0.5,-3,-1,7}, got[4]={0}, want[4]={0}; \
 if(rn==rm) memcpy(b,a,sizeof(a)); \
 if(rd==rm) memcpy(b,c,sizeof(c)); \
 if(rd==rn) memcpy(a,c,sizeof(c)); \
 if(rn==rm && rd==rn) memcpy(b,c,sizeof(c)); \
 for(int i=0;i<n;i++) want[i]=fma(sgn a[i],b[lane],c[i]); \
 __asm__ volatile("b 1f\n 1: ldr q" #rd ",[%3]\n ldr q" #rn ",[%0]\n ldr q" #rm ",[%1]\n ldr q" #rd ",[%3]\n " op " v" #rd "." form ",v" #rn "." form ",v" #rm "." scalar "[" #lane "]\n str q" #rd ",[%2]\n b 2f\n 2:" \
 ::"r"(rd==rn?c:a),"r"(rd==rm?c:b),"r"(got),"r"(c):"v26","v27","v31","memory"); \
 ++checks; if(memcmp(got,want,16)) {printf("FAIL %s %s lane %d dst %d src %d scalar %d\n",op,form,lane,rd,rn,rm); ++failures;} \
}while(0)
#define SF(op,sgn,l) TESTF(op,sgn,"4s","s",l,27,26,31,float,4,fmaf); TESTF(op,sgn,"2s","s",l,27,26,31,float,2,fmaf); \
 TESTF(op,sgn,"4s","s",l,31,26,31,float,4,fmaf); TESTF(op,sgn,"4s","s",l,26,26,31,float,4,fmaf)
#define DF(op,sgn,l) TESTF(op,sgn,"2d","d",l,27,26,31,double,2,fma); TESTF(op,sgn,"2d","d",l,31,26,31,double,2,fma); \
 TESTF(op,sgn,"2d","d",l,26,26,31,double,2,fma)
#define S(l) TEST("4s","s",l,27,26,31,float,4); TEST("2s","s",l,27,26,31,float,2); TEST("4s","s",l,26,26,31,float,4); TEST("2s","s",l,31,26,31,float,2); TEST("4s","s",l,31,31,31,float,4)
#define D(l) TEST("2d","d",l,27,26,31,double,2); TEST("2d","d",l,26,26,31,double,2); TEST("2d","d",l,31,26,31,double,2); TEST("2d","d",l,31,31,31,double,2)
static void *worker(void *arg) { return arg; }
int main(int argc, char **argv) {
 if(argc>1 && strcmp(argv[1],"--mt")==0) {
  pthread_t thread;
  if(pthread_create(&thread,NULL,worker,NULL) || pthread_join(thread,NULL))return 2;
 }
 S(0);S(1);S(2);S(3);D(0);D(1);
 SF("fmla",+,0);SF("fmla",+,1);SF("fmla",+,2);SF("fmla",+,3);SF("fmls",-,0);SF("fmls",-,1);SF("fmls",-,2);SF("fmls",-,3);
 DF("fmla",+,0);DF("fmla",+,1);DF("fmls",-,0);DF("fmls",-,1);
 if(failures)return 1;
 printf("simd_fp_indexed: ALL PASS (%d checks)\n",checks);return 0;
}
