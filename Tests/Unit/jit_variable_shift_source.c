// zstd keeps packed Huffman bits in the same value as a variable shift count.
// Shifts must mask only the effective count, preserving the live source.
#include <stdint.h>
#include <stdio.h>

#define RUN(op, reg, input, count, result, saved) do { \
    uint64_t words[2] = {0,0}; \
    __asm__ volatile( \
        "ldr x5, [%[count]]\n" \
        "mov x1, %[input]\n" \
        op " " reg "1, " reg "1, " reg "5\n" \
        "str x1, [%[out]]\n" \
        "orr x1, x1, x5\n" \
        "str x5, [%[out], #8]\n" \
        : : [count] "r"(&(count)), [input] "r"((uint64_t)(input)), \
            [out] "r"(words) : "x1", "x5", "memory"); \
    (result)=words[0]; (saved)=words[1]; \
} while (0)

static uint64_t asr(uint64_t x, unsigned count, unsigned width) {
    if (width==32) x=(uint32_t)x;
    count &= width-1;
    if (!count) return x;
    uint64_t result=x>>count;
    if (x & (UINT64_C(1)<<(width-1)))
        result |= (~UINT64_C(0) << (width-count));
    return width==32 ? (uint32_t)result : result;
}

static uint64_t ror(uint64_t x, unsigned count, unsigned width) {
    if (width==32) x=(uint32_t)x;
    count &= width-1;
    uint64_t result=count ? (x>>count)|(x<<(width-count)) : x;
    return width==32 ? (uint32_t)result : result;
}

int main(void) {
    uint64_t input=UINT64_C(0xfedcba9887654321);
    unsigned checks=0;
    for (unsigned i=0;i<256;i++) {
        uint64_t count=UINT64_C(0x34e3ca1280000000)|i;
        for (unsigned width=32;width<=64;width+=32) {
            for (unsigned op=0;op<4;op++) {
                uint64_t got,saved,want;
                if (width==64) {
                    if (op==0) { RUN("lsl","x",input,count,got,saved); want=input<<(i&63); }
                    else if (op==1) { RUN("lsr","x",input,count,got,saved); want=input>>(i&63); }
                    else if (op==2) { RUN("asr","x",input,count,got,saved); want=asr(input,i,64); }
                    else { RUN("ror","x",input,count,got,saved); want=ror(input,i,64); }
                } else {
                    if (op==0) { RUN("lsl","w",input,count,got,saved); want=(uint32_t)input<<(i&31); }
                    else if (op==1) { RUN("lsr","w",input,count,got,saved); want=(uint32_t)input>>(i&31); }
                    else if (op==2) { RUN("asr","w",input,count,got,saved); want=asr(input,i,32); }
                    else { RUN("ror","w",input,count,got,saved); want=ror(input,i,32); }
                }
                if (got!=want || saved!=count) {
                    printf("FAIL width=%u op=%u count=%llx result=%llx want=%llx saved=%llx\n",
                        width,op,(unsigned long long)count,(unsigned long long)got,
                        (unsigned long long)want,(unsigned long long)saved);
                    return 1;
                }
                checks++;
            }
        }
    }
    printf("ALL PASS: %u variable shifts preserve their count source\n",checks);
    return 0;
}
