/* Exact remaining Doom-profile opcodes: FRINTA + FCVTAS, then scalar lane copies.
 * Two million iterations per phase; outputs verify ties-away and upper clearing.
 * This is a throughput probe, not a whole-game FPS benchmark. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){uint64_t n=2000000,result;double x=2.5,y=0,t=now();
__asm__ volatile("ldr d0,[%2]\n1:\n frinta d30,d0\n fcvtas x1,d0\n subs %0,%0,#1\n b.ne 1b\n mov %1,x1\n str d30,[%3]":"+&r"(n),"=&r"(result):"r"(&x),"r"(&y):"x1","v0","v30","cc","memory");
printf("round_pair seconds=%.6f result=%llu,%.1f\n",now()-t,(unsigned long long)result,y);if(result!=3||y!=3)return 1;
float v[4]={2,3,5,7},o[4];n=2000000;t=now();
__asm__ volatile("ldr q27,[%1]\n1:\n mov s28,v27.s[1]\n mov s29,v27.s[2]\n mov s30,v27.s[3]\n subs %0,%0,#1\n b.ne 1b\n str q30,[%2]":"+&r"(n):"r"(v),"r"(o):"v27","v28","v29","v30","cc","memory");
printf("lane_copy seconds=%.6f result=%.1f,%.1f,%.1f,%.1f\n",now()-t,o[0],o[1],o[2],o[3]);return !(o[0]==7&&o[1]==0&&o[2]==0&&o[3]==0);}
