/* All AdvSIMD FMOV immediate encodings, including Doom 3's .2s #0.5.
 * Expected IEEE bits use the sign/exponent/fraction fields directly. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int failures;
static void check(unsigned imm, unsigned form, const uint64_t out[2]) {
 unsigned b6=(imm>>6)&1;
 unsigned exp32=((b6^1)<<7)|(b6?0x7c:0)|((imm>>4)&3);
 unsigned exp64=((b6^1)<<10)|(b6?0x3fc:0)|((imm>>4)&3);
 uint32_t f=((imm>>7)<<31)|(exp32<<23)|((imm&15)<<19);
 uint64_t d=((uint64_t)(imm>>7)<<63)|((uint64_t)exp64<<52)|((uint64_t)(imm&15)<<48);
 uint64_t lo=form==2?d:((uint64_t)f<<32)|f;
 uint64_t hi=form==0?0:lo;
 if(out[0]!=lo || out[1]!=hi) {
  if(failures++<8) printf("FAIL imm=%02x form=%u got=%016llx:%016llx expected=%016llx:%016llx\n",imm,form,(unsigned long long)out[1],(unsigned long long)out[0],(unsigned long long)hi,(unsigned long long)lo);
 }
}
#define ENCODE(i) (0x0f00f400u | (((i)&0xe0u)<<11) | (((i)&31u)<<5))
#define ONE(i) do { uint64_t out[2]; \
 __asm__ volatile(".inst %c[op]\n str q0,[%[out]]"::[op]"i"(ENCODE(i)),[out]"r"(out):"v0","memory"); check(i,0,out); \
 __asm__ volatile(".inst %c[op]\n str q0,[%[out]]"::[op]"i"(ENCODE(i)|0x40000000u),[out]"r"(out):"v0","memory"); check(i,1,out); \
 __asm__ volatile(".inst %c[op]\n str q0,[%[out]]"::[op]"i"(ENCODE(i)|0x60000000u),[out]"r"(out):"v0","memory"); check(i,2,out); \
 } while(0)
#define ROW(i) ONE(i);ONE(i+1);ONE(i+2);ONE(i+3);ONE(i+4);ONE(i+5);ONE(i+6);ONE(i+7);ONE(i+8);ONE(i+9);ONE(i+10);ONE(i+11);ONE(i+12);ONE(i+13);ONE(i+14);ONE(i+15)
int main(void) {
 ROW(0x00);
 ROW(0x10);
 ROW(0x20);
 ROW(0x30);
 ROW(0x40);
 ROW(0x50);
 ROW(0x60);
 ROW(0x70);
 ROW(0x80);
 ROW(0x90);
 ROW(0xa0);
 ROW(0xb0);
 ROW(0xc0);
 ROW(0xd0);
 ROW(0xe0);
 ROW(0xf0);
 if(failures) return 1;
 puts("simd_fmov_imm: ALL PASS (768 checks)"); return 0;
}
