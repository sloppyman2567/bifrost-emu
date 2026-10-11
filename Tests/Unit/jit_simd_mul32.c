/* SIMD 32-bit MUL must emit PMULLD (66 0F 38 40), not the invalid 5F opcode.
 * Frozen low-32-bit products exercise overflow, negative bit patterns,
 * Q=0 upper clearing, Q=1 and destination/source aliasing. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const uint32_t a[4]={0xffffffff,0x80000000,0x12345678,0x00010001};
static const uint32_t b[4]={3,2,0x100,0x00010001};
static const uint32_t expected[4]={0xfffffffd,0,0x34567800,0x00020001};
#define TEST(form, dest, want) do { \
 uint32_t out[4]={0}; \
 __asm__ volatile("ldr q0,[%[a]]\n ldr q1,[%[b]]\n movi v2.16b,#255\n " \
                  form "\n str " dest ",[%[out]]\n" \
                  ::[a]"r"(a),[b]"r"(b),[out]"r"(out):"v0","v1","v2","memory"); \
 if(memcmp(out,want,sizeof(out))) {printf("FAIL: %s\n",form); return 1;} \
 } while(0)
int main(void) {
 const uint32_t short_expected[4]={0xfffffffd,0,0,0};
 TEST("mul v2.4s,v0.4s,v1.4s","q2",expected);
 TEST("mul v0.4s,v0.4s,v1.4s","q0",expected);
 TEST("mul v1.4s,v0.4s,v1.4s","q1",expected);
 TEST("mul v2.2s,v0.2s,v1.2s","q2",short_expected);
 TEST("mul v0.2s,v0.2s,v1.2s","q0",short_expected);
 TEST("mul v1.2s,v0.2s,v1.2s","q1",short_expected);
 puts("simd_mul32: ALL PASS (6 checks)"); return 0;
}
