// test_madv_remove.c — madvise(MADV_REMOVE) follows mm/madvise.c
// madvise_remove: a writable shared file mapping gets a hole punched
// (pages read back as zero, file size kept), private anonymous memory
// fails with EINVAL and keeps its bytes, a read-only shared mapping fails
// with EACCES, and an unmapped range fails with ENOMEM.
//
// ART's CMC arena release calls MADV_REMOVE and zeroes the arena itself
// only when that fails with EINVAL. Returning success without zeroing
// left stale strings in recycled arenas, which then became DexCache type
// entries (SIGSEGV at a "Alg." string-valued class pointer).
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MADV_REMOVE
#define MADV_REMOVE 9
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int failures;
static long page;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int all_bytes(const unsigned char* p, size_t n, unsigned char value) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != value) return 0;
    return 1;
}

static int remove_errno(void* addr, size_t len) {
    errno = 0;
    return madvise(addr, len, MADV_REMOVE) == 0 ? 0 : errno;
}

// A memfd of three pages: 0x11, 0x22, 0x33.
static int make_file(void) {
    int fd = (int)syscall(SYS_memfd_create, "madv_remove", 0);
    if (fd < 0 || ftruncate(fd, 3 * page) != 0) return -1;
    unsigned char* buf = malloc(3 * page);
    for (int i = 0; i < 3; ++i) memset(buf + i * page, 0x11 * (i + 1), page);
    ssize_t n = pwrite(fd, buf, 3 * page, 0);
    free(buf);
    return n == 3 * page ? fd : -1;
}

// Punch the middle page of a shared mapping at hint (NULL = anywhere).
static void shared_case(const char* name, void* hint) {
    char what[128];
    int fd = make_file();
    int flags = MAP_SHARED | (hint ? MAP_FIXED_NOREPLACE : 0);
    unsigned char* p = mmap(hint, 3 * page, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (fd < 0 || p == MAP_FAILED || (hint && p != hint)) {
        printf("SKIP %s: mapping failed (%s)\n", name, strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    snprintf(what, sizeof what, "%s: remove middle page succeeds", name);
    check(remove_errno(p + page, page) == 0, what);
    snprintf(what, sizeof what, "%s: removed page reads zero", name);
    check(all_bytes(p + page, page, 0), what);
    snprintf(what, sizeof what, "%s: neighbours keep their bytes", name);
    check(all_bytes(p, page, 0x11) && all_bytes(p + 2 * page, page, 0x33), what);
    unsigned char* file = malloc(page);
    snprintf(what, sizeof what, "%s: hole is in the file, size kept", name);
    check(pread(fd, file, page, page) == page && all_bytes(file, page, 0) &&
          lseek(fd, 0, SEEK_END) == 3 * page, what);
    free(file);
    // Writes after the punch land in the file again.
    p[page] = 0x5a;
    unsigned char byte = 0;
    snprintf(what, sizeof what, "%s: later writes still reach the file", name);
    check(pread(fd, &byte, 1, page) == 1 && byte == 0x5a, what);
    munmap(p, 3 * page);
    close(fd);
}

int main(void) {
    page = sysconf(_SC_PAGESIZE);

    // Private anonymous: no file to punch. Linux leaves the bytes alone.
    unsigned char* anon = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(anon, 0xa5, 2 * page);
    check(remove_errno(anon, 2 * page) == EINVAL, "private anonymous: EINVAL");
    check(all_bytes(anon, 2 * page, 0xa5), "private anonymous: bytes untouched");
    // ART's fallback after EINVAL.
    check(madvise(anon, 2 * page, MADV_DONTNEED) == 0 && all_bytes(anon, 2 * page, 0),
          "private anonymous: MADV_DONTNEED fallback zeroes");

    shared_case("shared memfd", NULL);
    // Above bifrost's 4 GiB direct window (sparse page path).
    shared_case("shared memfd high", (void*)(uintptr_t)0x6000000000ULL);

    // Read-only fd: the shared mapping cannot be written, so EACCES.
    char path[] = "/tmp/madv_remove_XXXXXX";
    int fd = mkstemp(path);
    unsigned char* bytes = malloc(page);
    memset(bytes, 0x11, page);
    int ro = fd >= 0 && write(fd, bytes, page) == page ? open(path, O_RDONLY) : -1;
    free(bytes);
    if (fd >= 0) unlink(path);
    unsigned char* rop = ro < 0 ? MAP_FAILED : mmap(NULL, page, PROT_READ, MAP_SHARED, ro, 0);
    if (rop == MAP_FAILED) {
        printf("SKIP read-only shared: %s\n", strerror(errno));
    } else {
        check(remove_errno(rop, page) == EACCES, "read-only shared: EACCES");
        check(all_bytes(rop, page, 0x11), "read-only shared: bytes untouched");
        munmap(rop, page);
    }
    if (ro >= 0) close(ro);
    if (fd >= 0) close(fd);

    // Unmapped range.
    unsigned char* gone = mmap(NULL, page, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(gone, page);
    check(remove_errno(gone, page) == ENOMEM, "unmapped: ENOMEM");

    // A shared page followed by a hole: the page is punched, then ENOMEM.
    fd = make_file();
    unsigned char* two = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    munmap(two + page, page);
    check(remove_errno(two, 2 * page) == ENOMEM, "shared then hole: ENOMEM");
    check(all_bytes(two, page, 0), "shared then hole: mapped page still punched");
    munmap(two, page);
    close(fd);

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
