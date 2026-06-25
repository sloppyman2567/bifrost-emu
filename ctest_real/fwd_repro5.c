// fwd_repro5.c — debug FP value loading
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void) {
    // Load 3.14 directly from inline constant (no ldr d8)
    double d = 3.14;
    printf("d=%f\n", d);

    // Check the bit pattern
    uint64_t bits;
    memcpy(&bits, &d, 8);
    printf("bits=0x%lx (expected 0x40091eb851eb851f)\n", bits);

    return 0;
}
