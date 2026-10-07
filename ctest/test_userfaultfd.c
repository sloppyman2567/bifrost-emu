// test_userfaultfd.c — userfaultfd SIGBUS mode as ART's CMC GC uses it.
//
// ART (art/runtime/gc/collector/mark_compact.cc) moves heap pages away with
// mremap(MREMAP_DONTUNMAP), registers the emptied range MISSING-mode with
// UFFD_FEATURE_SIGBUS, and fills pages from its SIGBUS handler with
// UFFDIO_COPY / UFFDIO_ZEROPAGE. This checks that sequence, the access paths
// (interpreted, JIT-hot loop, kernel copy) and Linux's error conventions.
// Portable: the same source runs natively as a host-kernel oracle.
//
// Build: make setup-tests. Exit 0 + "ALL PASS" = pass.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } } while (0)

enum { PAGES = 4 };
static size_t page;
static int uffd = -1;
static char *heap, *from;  // registered range and its moved-away contents
static volatile int faults, bad_faults;
static volatile uintptr_t last_addr;
static volatile int zero_fill;  // handler uses UFFDIO_ZEROPAGE instead of COPY

static void on_sigbus(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)context;
    uintptr_t addr = (uintptr_t)info->si_addr;
    if (info->si_code != BUS_ADRERR || addr < (uintptr_t)heap ||
        addr >= (uintptr_t)heap + PAGES * page) {
        bad_faults++;
        _exit(3);  // a wrong signal would otherwise retry forever
    }
    faults++;
    last_addr = addr;
    uintptr_t at = addr & ~(uintptr_t)(page - 1);
    if (zero_fill) {
        struct uffdio_zeropage z = {.range = {.start = at, .len = page}};
        if (ioctl(uffd, UFFDIO_ZEROPAGE, &z)) _exit(4);
    } else {
        struct uffdio_copy c = {.dst = at, .src = (uintptr_t)from + (at - (uintptr_t)heap),
                                .len = page};
        if (ioctl(uffd, UFFDIO_COPY, &c)) _exit(5);
    }
}

static int open_uffd(void) {
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
    if (fd < 0 && errno == EINVAL) fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC);
    return fd;
}

__attribute__((noinline)) static long sum_pages(const volatile char *p) {
    long sum = 0;
    for (int i = 0; i < PAGES; i++) sum += p[i * page + 8];
    return sum;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    page = (size_t)sysconf(_SC_PAGESIZE);
    errno = 0;
    // Unknown flags (with USER_MODE_ONLY: the permission check runs first).
    CHECK(syscall(SYS_userfaultfd, 0x40000000 | UFFD_USER_MODE_ONLY) == -1 && errno == EINVAL);

    // KernelSupportsUffd(): probe with no features, then a second API fails.
    int probe = open_uffd();
    CHECK(probe >= 0);
    errno = 0;
    CHECK(ioctl(probe, UFFDIO_REGISTER, &(struct uffdio_register){0}) == -1 && errno == EINVAL);
    struct uffdio_api api = {.api = UFFD_API};
    CHECK(ioctl(probe, UFFDIO_API, &api) == 0);
    CHECK(api.features & UFFD_FEATURE_SIGBUS);
    CHECK((api.ioctls & (1ull << _UFFDIO_REGISTER)) && (api.ioctls & (1ull << _UFFDIO_API)));
    api = (struct uffdio_api){.api = UFFD_API};
    errno = 0;
    CHECK(ioctl(probe, UFFDIO_API, &api) == -1 && errno == EINVAL && api.features == 0);
    close(probe);
    probe = open_uffd();
    api = (struct uffdio_api){.api = 0x55};
    errno = 0;
    CHECK(ioctl(probe, UFFDIO_API, &api) == -1 && errno == EINVAL);
    close(probe);

    uffd = open_uffd();
    CHECK(uffd >= 0);
    api = (struct uffdio_api){.api = UFFD_API, .features = UFFD_FEATURE_SIGBUS};
    CHECK(ioctl(uffd, UFFDIO_API, &api) == 0);

    struct sigaction sa = {.sa_sigaction = on_sigbus, .sa_flags = SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGBUS, &sa, NULL) == 0);

    // A JIT-hot copy of sum_pages, warmed on an ordinary buffer.
    char *warm = mmap(NULL, PAGES * page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long warm_sum = 0;
    for (int i = 0; i < PAGES; i++) warm[i * page + 8] = 1;
    for (int r = 0; r < 20000; r++) warm_sum += sum_pages(warm);
    CHECK(warm_sum == 20000L * PAGES);

    // ART's compaction pause: DONTUNMAP the heap away, register the empty range.
    heap = mmap(NULL, PAGES * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(heap != MAP_FAILED);
    for (int i = 0; i < PAGES; i++) memset(heap + i * page, 10 + i, page);
    errno = 0;
    // DONTUNMAP reads new_address as a hint, so always pass one (ART: NULL).
    CHECK(mremap(heap, PAGES * page, 2 * PAGES * page, MREMAP_MAYMOVE | MREMAP_DONTUNMAP,
                 NULL) == MAP_FAILED && errno == EINVAL);  // sizes must match
    // Raw syscall: musl's wrapper passes new_address only with MREMAP_FIXED.
    errno = 0;
    CHECK(syscall(SYS_mremap, heap, PAGES * page, PAGES * page,
                  MREMAP_MAYMOVE | MREMAP_DONTUNMAP, 1) == -1 && errno == EINVAL);
    from = mremap(heap, PAGES * page, PAGES * page, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL);
    CHECK(from != MAP_FAILED && from != heap);
    CHECK(from[0] == 10 && from[3 * page + 5] == 13);
    struct uffdio_register reg = {.range = {.start = (uintptr_t)heap, .len = PAGES * page},
                                  .mode = UFFDIO_REGISTER_MODE_MISSING};
    CHECK(ioctl(uffd, UFFDIO_REGISTER, &reg) == 0);
    CHECK(reg.ioctls & (1ull << _UFFDIO_COPY));
    CHECK(reg.ioctls & (1ull << _UFFDIO_ZEROPAGE));
    struct uffdio_register bad = reg;
    bad.mode = 0;
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_REGISTER, &bad) == -1 && errno == EINVAL);
    bad = reg;
    bad.range.start += 1;
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_REGISTER, &bad) == -1 && errno == EINVAL);

    // Interpreted touch, then the JIT-hot loop over the other pages.
    CHECK(heap[5] == 10 && faults == 1 && last_addr == (uintptr_t)heap + 5);
    CHECK(sum_pages(heap) == 10 + 11 + 12 + 13 && faults == PAGES);
    CHECK(sum_pages(heap) == 46 && faults == PAGES);  // all present now
    heap[page] = 99;  // present pages accept writes without faults
    CHECK(heap[page] == 99 && faults == PAGES);

    // Filling a present page is EEXIST; the result field reports it.
    struct uffdio_copy copy = {.dst = (uintptr_t)heap, .src = (uintptr_t)from, .len = page};
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_COPY, &copy) == -1 && errno == EEXIST && copy.copy == -EEXIST);
    // Outside any registered range: ENOENT.
    copy = (struct uffdio_copy){.dst = (uintptr_t)warm, .src = (uintptr_t)from, .len = page};
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_COPY, &copy) == -1 && errno == ENOENT && copy.copy == -ENOENT);
    copy = (struct uffdio_copy){.dst = (uintptr_t)heap + 1, .src = (uintptr_t)from, .len = page};
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_COPY, &copy) == -1 && errno == EINVAL);

    // MADV_DONTNEED empties a registered page again (ZeroAndReleaseMemory).
    CHECK(madvise(heap + 2 * page, 2 * page, MADV_DONTNEED) == 0);
    // Partial copy: page 2 is missing, page 3 is filled first. EAGAIN, copy = 1 page.
    copy = (struct uffdio_copy){.dst = (uintptr_t)heap + 3 * page,
                                .src = (uintptr_t)from + 3 * page, .len = page};
    CHECK(ioctl(uffd, UFFDIO_COPY, &copy) == 0 && copy.copy == (int64_t)page);
    copy = (struct uffdio_copy){.dst = (uintptr_t)heap + 2 * page,
                                .src = (uintptr_t)from + 2 * page, .len = 2 * page};
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_COPY, &copy) == -1 && errno == EAGAIN && copy.copy == (int64_t)page);
    CHECK(heap[2 * page] == 12 && heap[3 * page] == 13 && faults == PAGES);

    // The kernel reading a missing registered page fails with EFAULT.
    CHECK(madvise(heap + page, page, MADV_DONTNEED) == 0);
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    errno = 0;
    CHECK(write(pipe_fds[1], heap + page, 1) == -1 && errno == EFAULT && faults == PAGES);
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    // ZEROPAGE from the handler.
    zero_fill = 1;
    CHECK(heap[page + 7] == 0 && faults == PAGES + 1);
    zero_fill = 0;
    struct uffdio_zeropage zero = {.range = {.start = (uintptr_t)heap + page, .len = page}};
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_ZEROPAGE, &zero) == -1 && errno == EEXIST && zero.zeropage == -EEXIST);
    struct uffdio_range wake = {.start = (uintptr_t)heap, .len = page};
    CHECK(ioctl(uffd, UFFDIO_WAKE, &wake) == 0);

    // A forked child inherits no registration: its missing pages read zero.
    CHECK(madvise(heap, page, MADV_DONTNEED) == 0);
    pid_t child = fork();
    if (child == 0) _exit(heap[3] == 0 && faults == PAGES + 1 ? 0 : 1);
    int status = 0;
    CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);

    // Unregistered missing pages are ordinary zero pages.
    CHECK(ioctl(uffd, UFFDIO_UNREGISTER, &(struct uffdio_range){
        .start = (uintptr_t)heap, .len = PAGES * page}) == 0);
    CHECK(heap[3] == 0 && faults == PAGES + 1);
    copy = (struct uffdio_copy){.dst = (uintptr_t)heap, .src = (uintptr_t)from, .len = page};
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_COPY, &copy) == -1 && errno == ENOENT);

    // KernelPrepareRangeForUffd: MAYMOVE|FIXED|DONTUNMAP into a PROT_NONE
    // from-space reservation, which ART may place far above the heap.
    char *space = mmap((void *)0x7200000000ull, PAGES * page, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (space == MAP_FAILED)
        space = mmap(NULL, PAGES * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(space != MAP_FAILED);
    for (int i = 0; i < PAGES; i++) memset(heap + i * page, 20 + i, page);
    memset(heap + 2 * page, 0, page);  // an all-zero page moves too
    from = mremap(heap, PAGES * page, PAGES * page,
                  MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, space);
    CHECK(from == space);
    CHECK(from[0] == 20 && from[page + 9] == 21 && from[2 * page + 1] == 0 &&
          from[3 * page + 5] == 23);
    CHECK(heap[0] == 0 && heap[3 * page] == 0);  // unregistered: zero pages
    CHECK(madvise(heap, PAGES * page, MADV_DONTNEED) == 0);
    reg = (struct uffdio_register){.range = {.start = (uintptr_t)heap, .len = PAGES * page},
                                   .mode = UFFDIO_REGISTER_MODE_MISSING};
    CHECK(ioctl(uffd, UFFDIO_REGISTER, &reg) == 0);
    CHECK(heap[page + 9] == 21 && heap[3 * page + 5] == 23 && faults == PAGES + 3);
    CHECK(ioctl(uffd, UFFDIO_UNREGISTER, &(struct uffdio_range){
        .start = (uintptr_t)heap, .len = PAGES * page}) == 0);
    // And back: the high range becomes empty, the low one gets its bytes.
    from[0] = 77;
    CHECK(mremap(from, PAGES * page, PAGES * page,
                 MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, heap) == heap);
    CHECK(heap[0] == 77 && heap[page + 9] == 21 && heap[2 * page + 1] == 0 &&
          heap[3 * page + 5] == 23);
    CHECK(from[0] == 0 && from[page + 9] == 0 && from[3 * page + 5] == 0);

    // Linear alloc: registered BEFORE the move, both ranges far above the heap.
    char *arena = mmap((void *)0x7300000000ull, PAGES * page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    char *shadow = mmap((void *)0x7400000000ull, PAGES * page, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(arena != MAP_FAILED && shadow != MAP_FAILED);
    for (int i = 0; i < PAGES; i++) memset(arena + i * page, 30 + i, page);
    reg = (struct uffdio_register){.range = {.start = (uintptr_t)arena, .len = PAGES * page},
                                   .mode = UFFDIO_REGISTER_MODE_MISSING};
    CHECK(ioctl(uffd, UFFDIO_REGISTER, &reg) == 0);
    heap = arena;
    from = mremap(arena, PAGES * page, PAGES * page,
                  MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, shadow);
    CHECK(from == shadow && from[2 * page] == 32);
    int before = faults;
    CHECK(arena[page + 3] == 31 && faults == before + 1);
    CHECK(sum_pages(arena) == 30 + 31 + 32 + 33 && faults == before + PAGES);
    copy = (struct uffdio_copy){.dst = (uintptr_t)arena, .src = (uintptr_t)from, .len = page};
    errno = 0;
    CHECK(ioctl(uffd, UFFDIO_COPY, &copy) == -1 && errno == EEXIST);
    CHECK(madvise(arena + 2 * page, page, MADV_DONTNEED) == 0);
    zero_fill = 1;
    CHECK(arena[2 * page + 4] == 0 && faults == before + PAGES + 1);
    zero_fill = 0;
    CHECK(madvise(arena, page, MADV_DONTNEED) == 0);
    CHECK(ioctl(uffd, UFFDIO_UNREGISTER, &(struct uffdio_range){
        .start = (uintptr_t)arena, .len = PAGES * page}) == 0);
    CHECK(arena[1] == 0 && arena[3 * page] == 33 && faults == before + PAGES + 1);
    CHECK(bad_faults == 0);
    close(uffd);
    printf("userfaultfd: %d checks %s\n", checks, failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
