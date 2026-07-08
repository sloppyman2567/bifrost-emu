// test_strb_post.c — test strb w0, [x27], #1 (post-increment store)
// This is used by glibc's printf to store digits to the buffer.
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

int main() {
    char buf[16] = {0};
    char *p = buf;
    
    // Test 1: strb with post-increment
    __asm__ volatile(
        "mov x1, %[buf]\n"
        "mov w0, #'A'\n"
        "strb w0, [x1], #1\n"
        "mov w0, #'B'\n"
        "strb w0, [x1], #1\n"
        "mov w0, #'C'\n"
        "strb w0, [x1], #1\n"
        "mov w0, #'D'\n"
        "strb w0, [x1], #1\n"
        : [buf] "+r"(p)
        : 
        : "x0", "x1", "memory"
    );
    buf[4] = 0;
    printf("strb post-inc: '%s' (expect ABCD)\n", buf);
    
    // Test 2: strb with pre-index
    p = buf;
    __asm__ volatile(
        "mov x1, %[buf]\n"
        "mov w0, #'X'\n"
        "strb w0, [x1, #1]!\n"
        "mov w0, #'Y'\n"
        "strb w0, [x1, #1]!\n"
        "mov w0, #'Z'\n"
        "strb w0, [x1, #1]!\n"
        : [buf] "+r"(p)
        : 
        : "x0", "x1", "memory"
    );
    buf[4] = 0;
    printf("strb pre-inc:  '%s' (expect  XYZ)\n", buf);
    
    // Test 3: str (64-bit) with post-increment
    p = buf;
    uint64_t val = 0x44434241; // "ABCD" in little-endian
    __asm__ volatile(
        "mov x1, %[buf]\n"
        "str x0, [x1], #8\n"
        : [buf] "+r"(p)
        : "r"(val)
        : "x1", "memory"
    );
    buf[8] = 0;
    printf("str post-inc:  '%s' (expect ABCD)\n", buf);
    
    write(1, "[done]\n", 7);
    return 0;
}
