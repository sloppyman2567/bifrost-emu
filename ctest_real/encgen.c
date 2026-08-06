/* Emit single-structure LD1 encodings for every size+index. */
#include <stdio.h>
int main(void){
  unsigned long long buf[8];
  void* p = buf;
  __asm__ volatile(
    "ld1 {v0.b}[3], [%0]\n\t"
    "ld1 {v1.h}[2], [%0]\n\t"
    "ld1 {v2.s}[1], [%0]\n\t"
    "ld1 {v3.d}[1], [%0]\n\t"
    "ld1 {v4.b}[9], [%0]\n\t"
    "ld1 {v5.h}[7], [%0]\n\t"
    "ld1 {v6.s}[3], [%0]"
    :: "r"(p) : "memory");
  puts("covered");
  return 0;
}