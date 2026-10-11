// test_mem_guard.c — memory range-safety regressions.
//
// Covers the 1.5.5-alpha fixes:
//   1. munmap with a wrapping length is rejected with EINVAL (it used to
//      page-round into covering — and freeing — every live mapping).
//   2. MAP_FIXED_NOREPLACE refuses to overlap the main stack.
//   3. madvise over an enormous length returns promptly (it used to walk
//      one page at a time, ~2^52 iterations, hanging the emulator), and a
//      wrapping length is rejected with EINVAL like Linux do_madvise. ART
//      passes a negative length when its stack bounds are wrong; applying
//      it zeroed every page above the start, including libc's text.
//   4. Above-the-4GiB-window MAP_FIXED map/write/read/unmap cycles run
//      sequentially across several regions (sparse pages_ path).
//
// Build: make setup-tests
// Run:   ./bifrost-emu Tests/Unit/test_mem_guard.elf
// Exit 0 + "ALL PASS" = pass.
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// 1. munmap with a wrapping length must be rejected.
static int test_munmap_wrap(void) {
    errno = 0;
    long r = syscall(SYS_munmap, (void*)0x2000UL,
                     (size_t)0xFFFFFFFFFFFFF000UL);
    if (r != -1 || errno != EINVAL) return -1;
    // The heap must still be usable (no live mappings were freed).
    void* p = malloc(4096);
    if (!p) return -2;
    memset(p, 0xAB, 4096);
    free(p);
    return 0;
}

// 2. MAP_FIXED_NOREPLACE must not map over the main stack.
static int test_noreplace_stack(void) {
    char local;
    uintptr_t addr = (uintptr_t)&local & ~(uintptr_t)4095;
    errno = 0;
    void* p = mmap((void*)addr, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED) { munmap(p, 4096); return -1; }
    if (errno != EEXIST) return -2;
    return 0;
}

// 3. madvise: wrapping lengths fail, huge ones return promptly.
static int test_madvise_huge(void) {
    unsigned char* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return -1;
    page[0] = 0x5A;
    // start + PAGE_ALIGN(len) wraps past zero: EINVAL, nothing discarded.
    errno = 0;
    if (madvise((void*)0x100000000UL, (size_t)0xFFFFFFFFFFFFF000UL,
                MADV_DONTNEED) != -1 || errno != EINVAL) return -2;
    errno = 0;
    if (madvise((void*)0x1000UL, (size_t)-1, MADV_DONTNEED) != -1 ||
        errno != EINVAL) return -3;
    if (page[0] != 0x5A) return -4;
    // Huge but valid: Linux may report ENOMEM for unmapped holes.
    errno = 0;
    int r = madvise((void*)0x100000000UL, (size_t)1 << 46, MADV_DONTNEED);
    if (r != 0 && errno != ENOMEM) return -5;
    return munmap(page, 4096) ? -6 : 0;
}

// 4. Above-window MAP_FIXED r/w cycles. Kept single-threaded: concurrent
//    MAP_FIXED triggers the JIT's runtime chain-slot patching while other
//    vCPUs may be executing that code (a known race, NOT fixed here — see
//    Docs/TESTS.md). This validates the sparse pages_ path and
//    the above-window bump cursor a fixed mapping must advance.
#define NREGIONS 4
#define ITERS    12
#define REGION   (1u << 20)  // 1 MiB
static int test_high_fixed_rw(void) {
    for (int id = 0; id < NREGIONS; id++) {
        uint64_t base = 0x200000000ULL + (uint64_t)id * 0x4000000ULL;  // 8 GiB + id*64 MiB
        for (int it = 0; it < ITERS; it++) {
            void* p = mmap((void*)base, REGION, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (p != (void*)base) return -1;
            volatile unsigned char* b = (volatile unsigned char*)p;
            for (size_t i = 0; i < REGION; i += 4096)
                b[i] = (unsigned char)(i + id + it);
            for (size_t i = 0; i < REGION; i += 4096)
                if (b[i] != (unsigned char)(i + id + it)) return -2;
            if (munmap(p, REGION) != 0) return -3;
        }
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   // progress visible even on crash
    printf("=== memory guard test ===\n");
    int r1 = test_munmap_wrap();
    printf("munmap wrap: %s\n", r1 == 0 ? "ok" : "bad");
    int r2 = test_noreplace_stack();
    printf("noreplace stack: %s\n", r2 == 0 ? "ok" : "bad");
    int r3 = test_madvise_huge();
    printf("madvise huge: %s\n", r3 == 0 ? "ok" : "bad");
    int r4 = test_high_fixed_rw();
    printf("high fixed rw: %s\n", r4 == 0 ? "ok" : "bad");
    if (r1 || r2 || r3 || r4) {
        printf("FAIL (%d %d %d %d)\n", r1, r2, r3, r4);
        return 1;
    }
    printf("mem guard: ALL PASS\n");
    return 0;
}
