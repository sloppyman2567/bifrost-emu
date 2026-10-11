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

// A private file mapping must refault file bytes, not anonymous zeros.
// Trickcal discards portions of libunity.so before calling its initializers.
static void private_case(void* hint) {
    int fd = make_file();
    unsigned char* p = mmap(hint, 2 * page, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | (hint ? MAP_FIXED_NOREPLACE : 0), fd, page);
    check(fd >= 0 && p != MAP_FAILED, "private file: map with nonzero offset");
    if (p == MAP_FAILED || fd < 0) { if (fd >= 0) close(fd); return; }
    p[0] = 0xa5;
    unsigned char byte = 0;
    check(pread(fd, &byte, 1, page) == 1 && byte == 0x22,
          "private file: writes do not change the file");
    check(remove_errno(p, page) == EACCES, "private file: MADV_REMOVE is EACCES");
    check(close(fd) == 0, "private file: close descriptor before discard");
    check(madvise(p, 2 * page, MADV_DONTNEED) == 0 &&
          all_bytes(p, page, 0x22) && all_bytes(p + page, page, 0x33),
          "private file: discard restores file bytes after close");
    // Moving a dirty private mapping must retain both its COW data and its
    // backing file. Protection changes must survive the move as well.
    p[0] = 0x5a;
    check(mprotect(p + page, page, PROT_READ) == 0, "private file: read-only tail");
    void* target = mmap(NULL, 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char* moved = target == MAP_FAILED ? MAP_FAILED :
        mremap(p, 2 * page, 2 * page, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    check(moved == target && moved != MAP_FAILED, "private file: fixed remap");
    if (moved == MAP_FAILED) { munmap(p, 2 * page); return; }
    p = moved;
    check(p[0] == 0x5a && p[page] == 0x33, "private file: remap preserves COW bytes");
    check(madvise(p, page, MADV_DONTNEED) == 0 && all_bytes(p, page, 0x22),
          "private file: remap retains file backing");
    // Replacing the first page must detach only that page's file backing.
    check(mmap(p, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
               -1, 0) == p, "private file: partial anonymous replacement");
    p[0] = 0x7f;
    check(madvise(p, 2 * page, MADV_DONTNEED) == 0 &&
          all_bytes(p, page, 0) && all_bytes(p + page, page, 0x33),
          "private file: replacement zeroes, neighbour refaults file");
    munmap(p, 2 * page);
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

    private_case(NULL);
    private_case((void*)(uintptr_t)0x6100000000ULL);

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
    // O_RDONLY restricts shared writes, but private COW writes are allowed.
    unsigned char* private_ro = ro < 0 ? MAP_FAILED :
        mmap(NULL, page, PROT_READ, MAP_PRIVATE, ro, 0);
    int private_writable = private_ro != MAP_FAILED &&
        mprotect(private_ro, page, PROT_READ | PROT_WRITE) == 0;
    check(private_writable,
          "private read-only fd: writable COW protection allowed");
    if (private_writable) {
        private_ro[0] = 0x7a;
        unsigned char byte = 0;
        check(pread(ro, &byte, 1, 0) == 1 && byte == 0x11,
              "private read-only fd: COW leaves file unchanged");
        check(madvise(private_ro, page, MADV_DONTNEED) == 0 && private_ro[0] == 0x11,
              "private read-only fd: discard restores file");
    }
    if (private_ro != MAP_FAILED) munmap(private_ro, page);
    // Moving a mapping longer than the file must not read its EOF pages.
    unsigned char* eof = ro < 0 ? MAP_FAILED :
        mmap(NULL, 2 * page, PROT_READ, MAP_PRIVATE, ro, 0);
    void* eof_target = mmap(NULL, 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char* eof_moved = eof == MAP_FAILED || eof_target == MAP_FAILED ? MAP_FAILED :
        mremap(eof, 2 * page, 2 * page, MREMAP_MAYMOVE | MREMAP_FIXED, eof_target);
    check(eof_moved != MAP_FAILED && eof_moved[0] == 0x11,
          "private file: remap does not touch pages beyond EOF");
    if (eof_moved != MAP_FAILED) munmap(eof_moved, 2 * page);
    else {
        if (eof != MAP_FAILED) munmap(eof, 2 * page);
        if (eof_target != MAP_FAILED) munmap(eof_target, 2 * page);
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
