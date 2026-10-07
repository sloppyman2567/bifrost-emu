// test_mem_reservation.c — guest memory above 4 GiB behaves like Linux now
// that the whole guest address space is one host reservation: fixed
// anonymous mappings at 1 TiB, faults after munmap/mprotect, a 1 TiB
// PROT_NONE reservation with a committed piece, a MAP_SHARED memfd seen
// through a high and a low view, MADV_DONTNEED and mremap above 4 GiB.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int failures;
static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static sigjmp_buf fault_jmp;
static volatile int fault_code;
static void on_segv(int sig, siginfo_t* si, void* ctx) {
    (void)sig; (void)ctx;
    fault_code = si->si_code;
    siglongjmp(fault_jmp, 1);
}
// Returns 0 when the access succeeds, else the SIGSEGV si_code.
static int try_write(volatile uint8_t* p) {
    fault_code = 0;
    if (sigsetjmp(fault_jmp, 1)) return fault_code;
    *p = 1;
    return 0;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    const long page = sysconf(_SC_PAGESIZE);

    // Fixed anonymous mapping at 1 TiB.
    uint8_t* const high = (uint8_t*)0x10000000000ull;
    const size_t len = 4u << 20;
    uint8_t* m = mmap(high, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    check(m == high, "MAP_FIXED_NOREPLACE at 1 TiB");
    if (m != high) return 1;
    int zero = 1;
    for (size_t i = 0; i < len; i += 4096) zero &= m[i] == 0;
    check(zero, "fresh high pages read as zero");
    // A hot loop, so the JIT runs it inline above 4 GiB.
    for (size_t i = 0; i < len; i++) m[i] = (uint8_t)(i * 7);
    uint64_t sum = 0, want = 0;
    for (int rep = 0; rep < 20; rep++)
        for (size_t i = 0; i < len; i += 8) sum += *(uint64_t*)(m + i);
    for (int rep = 0; rep < 20; rep++)
        for (size_t i = 0; i < len; i += 8) {
            uint64_t v = 0;
            for (int b = 0; b < 8; b++) v |= (uint64_t)(uint8_t)((i + b) * 7) << (8 * b);
            want += v;
        }
    check(sum == want, "hot loop over high memory");

    // mprotect read-only → ACCERR; munmap → MAPERR.
    check(mprotect(m, page, PROT_READ) == 0, "mprotect high page read-only");
    check(try_write(m) == SEGV_ACCERR, "write to read-only high page faults ACCERR");
    check(try_write(m + page) == 0, "neighbouring page still writable");
    check(munmap(m, len) == 0, "munmap high range");
    check(try_write(m + 2 * page) == SEGV_MAPERR, "write after munmap faults MAPERR");

    // 1 TiB PROT_NONE reservation, a piece committed inside it.
    double t0 = now();
    uint8_t* r = mmap(NULL, 1ull << 40, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    check(r != MAP_FAILED, "1 TiB PROT_NONE reservation");
    if (r != MAP_FAILED) {
        uint8_t* piece = r + (512ull << 30);
        check(mprotect(piece, 1 << 20, PROT_READ | PROT_WRITE) == 0, "commit 1 MiB inside it");
        memset(piece, 0x5a, 1 << 20);
        check(piece[(1 << 20) - 1] == 0x5a, "committed piece is usable");
        check(try_write(piece - page) == SEGV_ACCERR, "reservation outside the piece faults ACCERR");
        check(munmap(r, 1ull << 40) == 0, "unmap the reservation");
        check(now() - t0 < 2.0, "reserve/commit/unmap of 1 TiB is fast");
    }

    // MAP_SHARED memfd through a high and a low view.
    int fd = (int)syscall(SYS_memfd_create, "resv", 0);
    check(fd >= 0 && ftruncate(fd, 2 * page) == 0, "memfd");
    uint8_t* hv = mmap((void*)0x20000000000ull, 2 * page, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    uint8_t* lv = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(hv == (uint8_t*)0x20000000000ull && lv != MAP_FAILED, "high and low shared views");
    if (hv != MAP_FAILED && lv != MAP_FAILED) {
        hv[page + 5] = 0x77;
        lv[3] = 0x42;
        check(lv[page + 5] == 0x77 && hv[3] == 0x42, "shared views stay coherent");
        uint8_t b = 0;
        check(pread(fd, &b, 1, page + 5) == 1 && b == 0x77, "file sees the high-view store");
        munmap(hv, 2 * page);
        munmap(lv, 2 * page);
    }
    close(fd);

    // MADV_DONTNEED and mremap above 4 GiB.
    uint8_t* d = mmap((void*)0x30000000000ull, 4 * page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    check(d == (uint8_t*)0x30000000000ull, "high mapping for madvise/mremap");
    if (d != MAP_FAILED) {
        memset(d, 0x11, 4 * page);
        check(madvise(d + page, page, MADV_DONTNEED) == 0 && d[page] == 0 &&
              d[2 * page] == 0x11, "MADV_DONTNEED zeroes only its pages");
        uint8_t* g = mremap(d, 4 * page, 64 * page, MREMAP_MAYMOVE);
        check(g != MAP_FAILED && g[0] == 0x11 && g[page] == 0 && g[3 * page] == 0x11 &&
              g[63 * page] == 0, "mremap grow keeps contents, new tail is zero");
        if (g != MAP_FAILED) munmap(g, 64 * page);
    }

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
