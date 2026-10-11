#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int fails, checks;
static void check(const char* name, const uint64_t got[2], const uint64_t want[2]) {
 ++checks; if(memcmp(got,want,16)){++fails;printf("FAIL %s got=%llx:%llx want=%llx:%llx\n",name,(unsigned long long)got[1],(unsigned long long)got[0],(unsigned long long)want[1],(unsigned long long)want[0]);}
}
#define UNARY(ins,input,expect) do { uint64_t out[2]; \
 __asm__ volatile("ldr q24,[%0]\n " ins "\n str q24,[%1]"::"r"(input),"r"(out):"v24","memory");check(ins,out,expect); }while(0)
static int condition(unsigned c,unsigned f){int n=(f>>3)&1,z=(f>>2)&1,carry=(f>>1)&1,v=f&1;switch(c){case 0:return z;case 1:return !z;case 2:return carry;case 3:return !carry;case 4:return n;case 5:return !n;case 6:return v;case 7:return !v;case 8:return carry&&!z;case 9:return !carry||z;case 10:return n==v;case 11:return n!=v;case 12:return !z&&n==v;case 13:return z||n!=v;default:return 1;}}
#define COMPARE(cond,code,type,input,expected) do { \
 for(unsigned f=0;f<16;f++){uint64_t result, flags=(uint64_t)f<<28; \
 __asm__ volatile("ldr q24,[%[in]]\n ldr q25,[%[in],#16]\n msr nzcv,%[flags]\n fccmpe " type "24," type "25,#9," cond "\n mrs %[out],nzcv" :[out]"=&r"(result):[in]"r"(input),[flags]"r"(flags):"v24","v25","cc","memory"); \
 uint64_t want=(uint64_t)(condition(code,f)?expected:9)<<28;++checks;if(result!=want){fails++;printf("FAIL fccmpe %s flags=%u got=%llx want=%llx\n",cond,f,(unsigned long long)result,(unsigned long long)want);}} \
}while(0)
#define CONDITIONS(type,in,want) do{COMPARE("eq",0,type,in,want);COMPARE("ne",1,type,in,want);COMPARE("cs",2,type,in,want);COMPARE("cc",3,type,in,want);COMPARE("mi",4,type,in,want);COMPARE("pl",5,type,in,want);COMPARE("vs",6,type,in,want);COMPARE("vc",7,type,in,want);COMPARE("hi",8,type,in,want);COMPARE("ls",9,type,in,want);COMPARE("ge",10,type,in,want);COMPARE("lt",11,type,in,want);COMPARE("gt",12,type,in,want);COMPARE("le",13,type,in,want);COMPARE("al",14,type,in,want);COMPARE("nv",15,type,in,want);}while(0)
int main(void){
 uint64_t f[2]={0xc01000003fc00000ULL,0xc10c000040900000ULL};
 uint64_t dl[2]={0x3ff8000000000000ULL,0xc002000000000000ULL};
 uint64_t dh[2]={0x4012000000000000ULL,0xc021800000000000ULL};
 UNARY("fcvtl v24.2d,v24.2s",f,dl);UNARY("fcvtl2 v24.2d,v24.4s",f,dh);
 uint64_t b[2]={0xff80aa5504030201ULL,0x8877665544332211ULL};
 uint64_t wl[2]={0x0400030002000100ULL,0xff008000aa005500ULL};
 uint64_t wh[2]={0x4400330022001100ULL,0x8800770066005500ULL};
 UNARY("shll v24.8h,v24.8b,#8",b,wl);UNARY("shll2 v24.8h,v24.16b,#8",b,wh);
 uint64_t all[2]={~0ULL,~0ULL},sum[2]={0xf8,0};UNARY("addv b24,v24.8b",all,sum);
 uint64_t fa[2]={0x3f8000013f800001ULL,0x3f8000013f800001ULL},fb[2]={0x3f7ffffe3f7ffffeULL,0x3f7ffffe3f7ffffeULL},fc[2]={0xbf800000bf800000ULL,0xbf800000bf800000ULL},out[2],want[2]={0xa8800000a8800000ULL,0xa8800000a8800000ULL};
 __asm__ volatile("ldr q24,[%0]\n ldr q25,[%1]\n ldr q26,[%2]\n fmla v24.4s,v25.4s,v26.s[0]\n str q24,[%3]"::"r"(fc),"r"(fa),"r"(fb),"r"(out):"v24","v25","v26","memory");check("indexed fused fmla",out,want);
 uint64_t pairs[4];for(int k=0;k<4;k++){
 pairs[0]=k==0?0x3f800000:k==1?0x40000000:k==2?0x40400000:0x7fc00000;pairs[1]=0;pairs[2]=0x40000000;pairs[3]=0;
 CONDITIONS("s",pairs,k==0?8:k==1?6:k==2?2:3);
 pairs[0]=k==0?0x3ff0000000000000ULL:k==1?0x4000000000000000ULL:k==2?0x4008000000000000ULL:0x7ff8000000000000ULL;pairs[2]=0x4000000000000000ULL;
 CONDITIONS("d",pairs,k==0?8:k==1?6:k==2?2:3);
 }
 printf("doom_gap: %s (%d checks)\n",fails?"FAIL":"ALL PASS",checks);return fails?1:0;
}
