// Prints "test nzcv value taken" for every branch test and input.
#include <stdint.h>
#include <stdio.h>
#include "branch_table.h"

typedef uint64_t (*bfn)(uint64_t nzcv, uint64_t x5);
extern const bfn g_btests[];

int main(void) {
    static const uint64_t vals[] = {0, 1, 0x80000000ull, 0xffffffffull, 0x100000000ull,
                                    0xffffffff00000000ull, 0x8000000000000000ull, ~0ull,
                                    0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull};
    for (int t = 0; t < NB; t++)
        for (uint64_t f = 0; f < 16; f++)
            for (unsigned v = 0; v < sizeof vals / sizeof vals[0]; v++)
                printf("%d %llx %u %llu\n", t, (unsigned long long)f, v,
                       (unsigned long long)g_btests[t](f << 28, vals[v]));
    return 0;
}
