/* isolate each SIMD packing instruction, loading known values from memory
 * so the source GPRs are guaranteed correct. */
#include <stdio.h>
#include <stdint.h>

int main(int argc, char** argv){
  volatile uint64_t W = 520, H = 320;
  volatile uint16_t HWDATA = 520;
  uint64_t a=0,b=0,c=0,d=0;
  /* fmov d0, x6 */
  __asm__ volatile(
    "mov x6, %1\n\tfmov d0, x6\n\tstr d0, [%0]"
    :: "r"(&a), "r"(W) : "x6","v0","memory");
  /* mov v0.h[1], w7 */
  __asm__ volatile(
    "fmov d0, xzr\n\tmov w7, %w1\n\tmov v0.h[1], w7\n\tstr d0, [%0]"
    :: "r"(&b), "r"(H) : "w7","v0","memory");
  /* ld1 {v0.h}[2], [x8] */
  __asm__ volatile(
    "fmov d0, xzr\n\tmov x8, %1\n\tld1 {v0.h}[2], [x8]\n\tstr d0, [%0]"
    :: "r"(&c), "r"(&HWDATA) : "x8","v0","memory");
  /* mov v0.h[3], v1.h[0] */
  __asm__ volatile(
    "fmov d0, xzr\n\tfmov s1, %w1\n\tmov v0.h[3], v1.h[0]\n\tstr d0, [%0]"
    :: "r"(&d), "r"(1u) : "v0","v1","memory");
  printf("fmov d0,x6       = %016llx (exp 0000000000000208)\n", (unsigned long long)a);
  printf("mov v0.h[1],w7   = %016llx (exp 0000000001400000)\n", (unsigned long long)b);
  printf("ld1 {v0.h}[2]    = %016llx (exp 0000000002080000 = 520 at h[2])\n", (unsigned long long)c);
  printf("mov v0.h[3],v1.h[0]= %016llx (exp 0000000001000000)\n", (unsigned long long)d);
  return 0;
}