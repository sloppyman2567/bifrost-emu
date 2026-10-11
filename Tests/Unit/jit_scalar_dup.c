/* Scalar DUP: size is the lowest set bit in imm5, not the highest. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const uint8_t src[16]={0x10,0x21,0x32,0x43,0x54,0x65,0x76,0x87,0x98,0xa9,0xba,0xcb,0xdc,0xed,0xfe,0x0f};
static int fail, checks;
static void check(const uint64_t out[2],unsigned size,unsigned lane) {
 ++checks;
 uint64_t want=0;memcpy(&want,src+size*lane,size);
 if(out[0]!=want || out[1]) {printf("FAIL size=%u lane=%u got=%llx:%llx expected=%llx\n",size,lane,(unsigned long long)out[1],(unsigned long long)out[0],(unsigned long long)want);fail++;}
}
#define TEST(size,lane,enc) do {uint64_t out[2]; \
 __asm__ volatile("ldr q24,[%[src]]\n .inst %c[op]\n str q24,[%[out]]"::[src]"r"(src),[out]"r"(out),[op]"i"(0x5e000400u|((enc)<<16)|(24<<5)|24):"v24","memory");check(out,size,lane); \
 __asm__ volatile("ldr q24,[%[src]]\n movi v25.16b,#0xa5\n .inst %c[op]\n str q25,[%[out]]"::[src]"r"(src),[out]"r"(out),[op]"i"(0x5e000400u|((enc)<<16)|(24<<5)|25):"v24","v25","memory");check(out,size,lane); \
 } while(0)
int main(void){
 TEST(1,0,1);
 TEST(1,1,3);
 TEST(1,2,5);
 TEST(1,3,7);
 TEST(1,4,9);
 TEST(1,5,11);
 TEST(1,6,13);
 TEST(1,7,15);
 TEST(1,8,17);
 TEST(1,9,19);
 TEST(1,10,21);
 TEST(1,11,23);
 TEST(1,12,25);
 TEST(1,13,27);
 TEST(1,14,29);
 TEST(1,15,31);
 TEST(2,0,2);
 TEST(2,1,6);
 TEST(2,2,10);
 TEST(2,3,14);
 TEST(2,4,18);
 TEST(2,5,22);
 TEST(2,6,26);
 TEST(2,7,30);
 TEST(4,0,4);
 TEST(4,1,12);
 TEST(4,2,20);
 TEST(4,3,28);
 TEST(8,0,8);
 TEST(8,1,24);
 if(fail) return 1;printf("scalar_dup: ALL PASS (%d checks)\n",checks);return 0;
}
