// File aliases must stay coherent across low/high guest address space and
// mapping lifecycle operations. ART's JIT uses separate RW and R/RX views.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
static int checks, failures;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #expr, errno); \
    ++failures; } } while (0)
static void *alias(int fd, uintptr_t address, int prot, size_t length, off_t offset) {
    void *p = mmap((void *)address, length, prot,
                  MAP_SHARED | MAP_FIXED_NOREPLACE, fd, offset);
    if (p == MAP_FAILED) { perror("alias"); exit(2); }
    return p;
}
int main(void) {
    const size_t page = 4096;
    int fd = syscall(SYS_memfd_create, "alias-test", 0);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 4 * page) == 0);
    // Goldfish's transport uses PROT_WRITE, then reads host-written replies.
    // Linux arm64 grants read access too, while the VMA remains write-only.
    volatile uint32_t *write_only = alias(fd, 0x7400000000ULL, PROT_WRITE, page, 0);
    write_only[0] = 42;
    CHECK(write_only[0] == 42);
    CHECK(mprotect((void *)write_only, page, PROT_NONE) == 0);
    CHECK(mprotect((void *)write_only, page, PROT_WRITE) == 0);
    CHECK(write_only[0] == 42);
    CHECK(munmap((void *)write_only, page) == 0);
    volatile uint32_t *low = alias(fd, 0x30000000, PROT_READ | PROT_WRITE, 3 * page, 0);
    volatile uint32_t *high = alias(fd, 0x7100000000ULL, PROT_READ | PROT_WRITE, 3 * page, 0);
    volatile uint32_t *view = alias(fd, 0x7200000000ULL, PROT_READ, 3 * page, 0);
    volatile uint32_t *offset = alias(fd, 0x7300000000ULL, PROT_READ, page, page);
    for (unsigned i = 0; i < 10000; ++i) {
        low[0] = i;
        CHECK(high[0] == i && view[0] == i);
        high[page / 4] = i + 7;
        CHECK(low[page / 4] == i + 7 && offset[0] == i + 7);
    }
    CHECK(msync((void *)high, 3 * page, MS_SYNC) == 0);
    uint32_t disk = 0;
    CHECK(pread(fd, &disk, sizeof(disk), page) == sizeof(disk));
    CHECK(disk == 10006);
    CHECK(madvise((void *)low, 3 * page, MADV_DONTNEED) == 0);
    CHECK(madvise((void *)high, 3 * page, MADV_DONTNEED) == 0);
    CHECK(high[page / 4] == 10006 && low[0] == 9999);
    // Replace one page with anonymous memory without zeroing the file or
    // detaching its neighbors. Also test low alias cleanup on munmap/reuse.
    CHECK(mmap((void *)(low + page / 4), page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)(low + page / 4));
    CHECK(low[page / 4] == 0 && high[page / 4] == 10006);
    low[2 * page / 4] = 51;
    CHECK(view[2 * page / 4] == 51);
    CHECK(munmap((void *)low, page) == 0);
    CHECK(mmap((void *)low, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)low);
    CHECK(low[0] == 0 && high[0] == 9999);
    // A closed guest descriptor must not invalidate surviving mappings.
    CHECK(close(fd) == 0);
    high[0] = 123;
    CHECK(view[0] == 123);
    void *moved = mremap((void *)high, 3 * page, 4 * page,
        MREMAP_MAYMOVE | MREMAP_FIXED, (void *)0x7400000000ULL);
    CHECK(moved == (void *)0x7400000000ULL);
    if (moved == MAP_FAILED) return 1;
    high = moved;
    high[0] = 456;
    CHECK(view[0] == 456);
    CHECK(high[3 * page / 4] == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { high[0] = 789; low[2 * page / 4] = 333; _exit(0); }
    if (pid > 0) {
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(view[0] == 789 && high[0] == 789);
        CHECK(view[2 * page / 4] == 333);
    }
    CHECK(mremap((void *)high, 4 * page, page, 0) == (void *)high);
    high[0] = 42;
    CHECK(view[0] == 42);
    CHECK(mremap((void *)high, page, 2 * page, 0) == (void *)high);
    moved = mremap((void *)high, 2 * page, 2 * page,
        MREMAP_MAYMOVE | MREMAP_FIXED, (void *)0x7600000000ULL);
    CHECK(moved == (void *)0x7600000000ULL);
    if (moved == MAP_FAILED) return 1;
    high = moved;
    high[page / 4] = 88;
    CHECK(offset[0] == 88);
    CHECK(munmap((void *)high, 2 * page) == 0);
    CHECK(munmap((void *)view, 3 * page) == 0);
    CHECK(munmap((void *)offset, page) == 0);
    CHECK(munmap((void *)low, 3 * page) == 0);
    // Read-only file descriptors must reject writable aliases without
    // evicting a live MAP_FIXED destination or faulting the host on access.
    char path[] = "/tmp/bifrost-shared-XXXXXX";
    fd = mkstemp(path);
    CHECK(fd >= 0 && ftruncate(fd, page) == 0);
    disk = 917;
    CHECK(pwrite(fd, &disk, sizeof(disk), 0) == sizeof(disk));
    int ro = open(path, O_RDONLY);
    CHECK(ro >= 0);
    volatile uint32_t *ro_low = alias(ro, 0x35000000, PROT_READ, page, 0);
    volatile uint32_t *ro_high = alias(ro, 0x7800000000ULL, PROT_READ, page, 0);
    CHECK(mprotect((void *)ro_low, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    CHECK(mprotect((void *)ro_high, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    CHECK(mmap((void *)ro_low, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
               ro, 0) == MAP_FAILED && errno == EACCES);
    CHECK(ro_low[0] == 917 && ro_high[0] == 917);
    CHECK(mprotect((void *)ro_low, page, PROT_NONE) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(ro_high[0] == 917 ? 0 : 1);
    if (pid > 0) {
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(mprotect((void *)ro_low, page, PROT_READ) == 0);
    CHECK(ro_low[0] == 917);
    CHECK(close(ro) == 0 && close(fd) == 0 && unlink(path) == 0);
    CHECK(munmap((void *)ro_low, page) == 0);
    CHECK(munmap((void *)ro_high, page) == 0);
    // Execute generated code through a high RX alias of a low RW mapping,
    // with a PC-relative literal: the exact shape of ART's JIT code cache.
    fd = syscall(SYS_memfd_create, "code-alias", 0);
    CHECK(fd >= 0 && ftruncate(fd, page) == 0);
    uint32_t *code = alias(fd, 0x34000000, PROT_READ | PROT_WRITE, page, 0);
    void *rx = alias(fd, 0x7500000000ULL, PROT_READ | PROT_EXEC, page, 0);
    code[0] = 0x58000040; // ldr x0, PC+8
    code[1] = 0xd65f03c0; // ret
    code[2] = 0x76543210;
    code[3] = 0xfedcba98;
    __builtin___clear_cache(rx, (char *)rx + 16);
    CHECK(close(fd) == 0);
    uint64_t (*generated)(void) = (uint64_t (*)(void))rx;
    for (unsigned i = 0; i < 2000; ++i)
        CHECK(generated() == UINT64_C(0xfedcba9876543210));
    CHECK(munmap(code, page) == 0);
    CHECK(munmap(rx, page) == 0);
    printf("test_shared_alias: %d checks passed, %d failures\n", checks - failures, failures);
    return failures != 0;
}
