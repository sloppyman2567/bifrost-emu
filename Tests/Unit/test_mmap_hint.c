#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    const size_t size = 64 * 4096;
    unsigned char *hint = (void *)(uintptr_t)0x68000000;
    for (int iteration = 0; iteration < 64; ++iteration) {
        unsigned char *first = mmap(hint + 17, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (first == MAP_FAILED) return 1;
        first[0] = 73; first[size - 1] = 91;
        if (first != hint) return 1;
        unsigned char *second = mmap(hint, size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (second == MAP_FAILED || second == first || first[0] != 73 || first[size - 1] != 91)
            return 2;
        if (second[0] || second[size - 1]) return 3;
        if (munmap(second, size) || munmap(first, size)) return 4;
    }
    const size_t reserve_size = 1024UL * 1024 * 1024;
    unsigned char *reserve_hint = (void *)(uintptr_t)0x80000000;
    unsigned char *reserve = mmap(reserve_hint, reserve_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserve == MAP_FAILED) return 5;
    if (mprotect(reserve, 4096, PROT_READ | PROT_WRITE) || reserve[0]) return 6;
    reserve[0] = 77;
    if (reserve != reserve_hint || munmap(reserve, reserve_size)) return 7;
    reserve = mmap(reserve_hint, reserve_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserve == MAP_FAILED || reserve[0]) return 8;
    if (reserve != reserve_hint || munmap(reserve, reserve_size)) return 9;
    puts("test_mmap_hint: ALL PASS");
    return 0;
}
