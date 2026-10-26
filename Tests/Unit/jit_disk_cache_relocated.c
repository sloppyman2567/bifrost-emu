/* The same file-backed code at different guest addresses, for the persistent
 * JIT cache (test_jit_disk_cache.sh). argv[1] is the address to map the code
 * at, argv[2] a file to hold it. The loop materializes the constant
 * 0x12345678 (MOVZ/MOVK), which lies within 4 GiB of a low mapping but not of
 * a high one: a translation recorded at one address must equal a fresh
 * compile at the other. The result is checked against the same loop in C. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* uint64_t kernel(uint64_t n): n iterations of acc = ror((acc ^ 0x12345678) + n, 7). */
static const uint32_t kernel_code[] = {
    0xd2800002,  /* mov  x2, #0 */
    0x528acf03,  /* movz w3, #0x5678 */
    0x72a24683,  /* movk w3, #0x1234, lsl #16 */
    0xca030042,  /* 1: eor x2, x2, x3 */
    0x8b000042,  /* add  x2, x2, x0 */
    0x93c21c42,  /* ror  x2, x2, #7 */
    0xf1000400,  /* subs x0, x0, #1 */
    0x54ffff81,  /* b.ne 1b */
    0xaa0203e0,  /* mov  x0, x2 */
    0xd65f03c0,  /* ret */
};

static uint64_t reference(uint64_t n) {
    uint64_t acc = 0;
    for (; n; --n) {
        acc = (acc ^ 0x12345678u) + n;
        acc = (acc >> 7) | (acc << 57);
    }
    return acc;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s ADDRESS FILE\n", argv[0]); return 2; }
    const uintptr_t at = strtoull(argv[1], NULL, 0);
    const int fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0600);
    const size_t size = 4096;
    uint8_t page[4096] = {0};
    memcpy(page, kernel_code, sizeof kernel_code);
    if (fd < 0 || write(fd, page, size) != (ssize_t)size) { perror("kernel file"); return 1; }
    void* code = mmap((void*)at, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (code != (void*)at) { perror("mmap"); return 1; }
    uint64_t (*kernel)(uint64_t) = (uint64_t (*)(uint64_t))code;
    for (uint64_t n = 1; n <= 200000; n += 9973) {
        if (kernel(n) != reference(n)) {
            printf("jit_disk_cache_relocated: FAIL n=%llu\n", (unsigned long long)n);
            return 1;
        }
    }
    close(fd);
    printf("jit_disk_cache_relocated: ALL PASS at %#lx\n", (unsigned long)at);
    return 0;
}
