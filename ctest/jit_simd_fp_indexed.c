/* Indexed FP multiply: high scalar lanes, high registers, aliases and Q=0. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int checks, failures;
#define TEST(form,scalar,lane,rd,rn,rm,type,n) do { \
 type a[4]={2,4,8,16}, b[4]={-1,2,-4,8}, got[4]={0}, want[4]={0}; \
 if(rn==rm) memcpy(b,a,sizeof(a)); \
 for(int i=0;i<n;i++) want[i]=a[i]*b[lane]; \
 __asm__ volatile("ldr q" #rn ",[%0]\n ldr q" #rm ",[%1]\n fmul v" #rd "." form ",v" #rn "." form ",v" #rm "." scalar "[" #lane "]\n str q" #rd ",[%2]" \
 ::"r"(a),"r"(b),"r"(got):"v26","v27","v31","memory"); \
 ++checks; if(memcmp(got,want,16)) {printf("FAIL %s lane %d dst %d src %d scalar %d\n",form,lane,rd,rn,rm); ++failures;} \
}while(0)
#define S(l) TEST("4s","s",l,27,26,31,float,4); TEST("2s","s",l,27,26,31,float,2); TEST("4s","s",l,26,26,31,float,4); TEST("2s","s",l,31,26,31,float,2); TEST("4s","s",l,31,31,31,float,4)
#define D(l) TEST("2d","d",l,27,26,31,double,2); TEST("2d","d",l,26,26,31,double,2); TEST("2d","d",l,31,26,31,double,2); TEST("2d","d",l,31,31,31,double,2)
int main(void) { S(0);S(1);S(2);S(3);D(0);D(1); if(failures)return 1;printf("simd_fp_indexed: ALL PASS (%d checks)\n",checks);return 0; }
