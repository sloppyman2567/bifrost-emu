/* SSHL/USHL use a signed low-byte count for every lane width. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define OP(name, word) static void name(void*n,void*s,void*out){ __asm__ volatile("ldr q0,[%0]\nldr q1,[%1]\n.inst " #word "\nstr q0,[%2]"::"r"(n),"r"(s),"r"(out):"v0","v1","memory"); }
OP(op_0_0_0,0x0e214400)
OP(op_0_0_1,0x2e214400)
OP(op_0_1_0,0x0e614400)
OP(op_0_1_1,0x2e614400)
OP(op_0_2_0,0x0ea14400)
OP(op_0_2_1,0x2ea14400)
OP(op_1_0_0,0x4e214400)
OP(op_1_0_1,0x6e214400)
OP(op_1_1_0,0x4e614400)
OP(op_1_1_1,0x6e614400)
OP(op_1_2_0,0x4ea14400)
OP(op_1_2_1,0x6ea14400)
OP(op_1_3_0,0x4ee14400)
OP(op_1_3_1,0x6ee14400)
static uint64_t expected(uint64_t value,int bits,int shift,int u) {
 uint64_t mask=bits==64?UINT64_MAX:((UINT64_C(1)<<bits)-1);value&=mask;
 if(shift>=0) return shift>=bits?0:(value<<shift)&mask;
 int count=-shift,neg=!u&&(value&(UINT64_C(1)<<(bits-1)));
 if(count>=bits)return neg?mask:0;
 uint64_t result=value>>count;
 if(neg)result|=mask^(mask>>count);
 return result;
}
int main(void){int checks=0;
struct Form {void(*op)(void*,void*,void*);int q,size,u;} forms[]={
{op_0_0_0,0,0,0},
{op_0_0_1,0,0,1},
{op_0_1_0,0,1,0},
{op_0_1_1,0,1,1},
{op_0_2_0,0,2,0},
{op_0_2_1,0,2,1},
{op_1_0_0,1,0,0},
{op_1_0_1,1,0,1},
{op_1_1_0,1,1,0},
{op_1_1_1,1,1,1},
{op_1_2_0,1,2,0},
{op_1_2_1,1,2,1},
{op_1_3_0,1,3,0},
{op_1_3_1,1,3,1}};
for(unsigned f=0;f<sizeof(forms)/sizeof(forms[0]);f++) {
 struct Form form=forms[f];int bytes=1<<form.size,bits=bytes*8,lanes=(form.q?16:8)/bytes;
 int64_t shifts[]={0,1,-1,bits-1,bits,-bits+1,-bits,-bits-1,127,-128,257,255,INT64_MIN};
 for(unsigned k=0;k<sizeof(shifts)/sizeof(shifts[0]);k++) {
  unsigned char n[16]={0},s[16]={0},out[16],want[16]={0};
  for(int lane=0;lane<lanes;lane++) {
   uint64_t value=(lane&1)?UINT64_MAX:UINT64_C(0x8123456789abcdef), count=shifts[k];
   memcpy(n+lane*bytes,&value,bytes);memcpy(s+lane*bytes,&count,bytes);
   int shift=(int8_t)(uint8_t)count;uint64_t result=expected(value,bits,shift,form.u);
   memcpy(want+lane*bytes,&result,bytes);
  }
  form.op(n,s,out);checks++;
  if(memcmp(out,want,16)){printf("FAIL Q=%d size=%d U=%d count=%lld\n",form.q,bytes,form.u,(long long)shifts[k]);return 1;}
 }
}
printf("%d checks passed\n",checks);return 0;}
