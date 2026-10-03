/* Vector FP compare-zero masks, including NaN, signed zero and Q=0 clearing. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define OP(n,w) static void n(void *in,void*out) { __asm__ volatile("ldr q1,[%0]\n.inst " #w "\nstr q0,[%1]"::"r"(in),"r"(out):"v0","v1","memory"); }
OP(op_0_0_eq,0x0ea0d820)
OP(op_0_0_ge,0x2ea0c820)
OP(op_0_0_gt,0x0ea0c820)
OP(op_0_0_le,0x2ea0d820)
OP(op_0_0_lt,0x0ea0e820)
OP(op_0_1_eq,0x4ea0d820)
OP(op_0_1_ge,0x6ea0c820)
OP(op_0_1_gt,0x4ea0c820)
OP(op_0_1_le,0x6ea0d820)
OP(op_0_1_lt,0x4ea0e820)
OP(op_1_1_eq,0x4ee0d820)
OP(op_1_1_ge,0x6ee0c820)
OP(op_1_1_gt,0x4ee0c820)
OP(op_1_1_le,0x6ee0d820)
OP(op_1_1_lt,0x4ee0e820)
int main(void){
 struct Form{void(*op)(void*,void*);int d,q,k;} forms[]={{op_0_0_eq,0,0,0},{op_0_0_ge,0,0,1},{op_0_0_gt,0,0,2},{op_0_0_le,0,0,3},{op_0_0_lt,0,0,4},{op_0_1_eq,0,1,0},{op_0_1_ge,0,1,1},{op_0_1_gt,0,1,2},{op_0_1_le,0,1,3},{op_0_1_lt,0,1,4},{op_1_1_eq,1,1,0},{op_1_1_ge,1,1,1},{op_1_1_gt,1,1,2},{op_1_1_le,1,1,3},{op_1_1_lt,1,1,4}};
 const uint64_t samples[]={0,0x8000000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8000000000001ull};
 const unsigned char expected[7][5]={
 {1,1,0,1,0},{1,1,0,1,0},{0,1,1,0,0},{0,0,0,1,1},
 {0,1,1,0,0},{0,0,0,1,1},{0,0,0,0,0}};
 int checks=0;
 for(unsigned f=0;f<sizeof(forms)/sizeof(forms[0]);f++)for(unsigned n=0;n<sizeof(samples)/sizeof(samples[0]);n++){
  double x;memcpy(&x,&samples[n],8);uint64_t input[2],out[2];
  if(forms[f].d) input[0]=input[1]=samples[n];
  else {float v=(float)x;uint32_t bits;memcpy(&bits,&v,4);input[0]=input[1]=(uint64_t)bits|((uint64_t)bits<<32);}
  forms[f].op(input,out);uint64_t want=expected[n][forms[f].k]?UINT64_MAX:0;
  if(out[0]!=want || out[1]!=(forms[f].q?want:0)){printf("FAIL form=%u sample=%u\n",f,n);return 1;}checks++;
 }
 printf("%d checks passed\n",checks);return 0;
}
