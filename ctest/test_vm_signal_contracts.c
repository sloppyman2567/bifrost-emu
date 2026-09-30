// Linux VM and alternate-stack ABI regressions. Native Linux is the
// independent reference; this does not claim Arm instruction conformance.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static int checks, failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            failures++;                                                                            \
            printf("FAIL %d: %s (errno=%d)\n", __LINE__, #x, errno);                               \
        }                                                                                          \
    } while (0)
static size_t p;
static int fd;
static int stop_reader, reader_failed, reader_count;
static void *reader(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&stop_reader, __ATOMIC_ACQUIRE)) {
        char b[32];
        if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, b, sizeof b) != sizeof b) {
            __atomic_store_n(&reader_failed, 1, __ATOMIC_RELEASE);
            break;
        }
        for (size_t i = 0; i < sizeof b; i++)
            if (b[i] != 'A')
                __atomic_store_n(&reader_failed, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&reader_count, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}
static volatile sig_atomic_t disable_rc, disable_errno, replace_rc, replace_errno, query_flags;
static stack_t replacement;
static void handler(int sig) {
    (void)sig;
    stack_t off = {.ss_flags = SS_DISABLE}, old;
    errno = 0;
    disable_rc = sigaltstack(&off, NULL);
    disable_errno = errno;
    errno = 0;
    replace_rc = sigaltstack(&replacement, NULL);
    replace_errno = errno;
    if (sigaltstack(NULL, &old) == 0)
        query_flags = old.ss_flags;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        return 2;
    p = (size_t)ps;
    char name[] = "/tmp/bifrost-vm-contracts-XXXXXX";
    fd = mkstemp(name);
    if (fd < 0)
        return 2;
    unlink(name);
    size_t bytes = 2 * 1024 * 1024 + p;
    char *data = malloc(bytes);
    if (!data)
        return 2;
    memset(data, 'B', bytes);
    memset(data, 'A', p);
    CHECK(write(fd, data, bytes) == (ssize_t)bytes);
    free(data);
    char *a = mmap(NULL, p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED)
        return 2;
    memset(a, 0x7d, p);
    CHECK(mmap(a, p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == a);
    CHECK(mprotect(a, p, PROT_READ | PROT_WRITE) == 0);
    int clean = 1;
    for (size_t i = 0; i < p; i++)
        if (a[i])
            clean = 0;
    CHECK(clean);
    CHECK(mmap(a, p, PROT_NONE, MAP_PRIVATE | MAP_FIXED, fd, p) == a);
    CHECK(mprotect(a, p, PROT_READ) == 0);
    CHECK(a[0] == 'B' && a[p - 1] == 'B');
    CHECK(munmap(a, p) == 0);
    CHECK(lseek(fd, 17, SEEK_SET) == 17);
    a = mmap(NULL, p, PROT_READ, MAP_PRIVATE, fd, p);
    CHECK(a != MAP_FAILED);
    CHECK(lseek(fd, 0, SEEK_CUR) == 17);
    if (a != MAP_FAILED) {
        CHECK(a[0] == 'B');
        munmap(a, p);
    }
    a = mmap(NULL, p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED)
        return 2;
    CHECK(munmap(a, p) == 0);
    CHECK(mmap(a, p, PROT_READ, MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, p) == a);
    CHECK(lseek(fd, 0, SEEK_CUR) == 17);
    CHECK(a[0] == 'B');
    CHECK(munmap(a, p) == 0);
    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL))
        return 2;
    while (!__atomic_load_n(&reader_count, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&reader_failed, __ATOMIC_ACQUIRE))
        sched_yield();
    int maps_ok = 1;
    for (int i = 0; i < 32; i++) {
        a = mmap(NULL, bytes - p, PROT_READ, MAP_PRIVATE, fd, p);
        if (a == MAP_FAILED) {
            maps_ok = 0;
            break;
        }
        if (a[0] != 'B' || a[bytes - p - 1] != 'B')
            maps_ok = 0;
        munmap(a, bytes - p);
    }
    __atomic_store_n(&stop_reader, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(maps_ok);
    CHECK(reader_count > 0);
    CHECK(!reader_failed);
    close(fd);
    char *arena = mmap(NULL, 4 * p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED)
        return 2;
    CHECK(munmap(arena, 4 * p) == 0);
    a = mmap(arena, p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    char *block =
        mmap(arena + p, p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (a != arena || block != arena + p)
        return 2;
    a[0] = 42;
    block[0] = 73;
    errno = 0;
    CHECK(mremap(a, p, 2 * p, 0) == MAP_FAILED && errno == ENOMEM);
    CHECK(a[0] == 42 && block[0] == 73);
    errno = 0;
    // Native Linux returns EINVAL. QEMU 11.1.50 instead returns ENOMEM;
    // keep the Linux assertion strict rather than accepting that mismatch.
    CHECK(mremap(a, p, 0, MREMAP_MAYMOVE) == MAP_FAILED && errno == EINVAL);
    errno = 0;
    CHECK(mremap(a, p, 2 * p, MREMAP_FIXED, arena + 2 * p) == MAP_FAILED && errno == EINVAL);
    errno = 0;
    CHECK(mremap(a, p, 2 * p, MREMAP_MAYMOVE | MREMAP_FIXED, a) == MAP_FAILED && errno == EINVAL);
    CHECK(a[0] == 42 && block[0] == 73);
    char *m = mremap(a, p, 2 * p, MREMAP_MAYMOVE);
    CHECK(m != MAP_FAILED && m != a);
    if (m == MAP_FAILED)
        return 2;
    CHECK(m[0] == 42 && m[p] == 0 && block[0] == 73);
    errno = 0;
    CHECK(mprotect(a, p, PROT_READ) == -1 && errno == ENOMEM);
    // The relocated source may occupy the reclaimed tail of arena.
    // Obtain a separate live destination instead of assuming its address.
    char *dest = mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dest == MAP_FAILED)
        return 2;
    memset(dest, 0x55, 2 * p);
    m[p - 1] = 0x21;
    CHECK(mremap(m, 2 * p, p, MREMAP_MAYMOVE | MREMAP_FIXED, dest) == dest);
    CHECK(dest[0] == 42 && dest[p - 1] == 0x21 && dest[p] == 0x55);
    errno = 0;
    CHECK(mprotect(m, p, PROT_READ) == -1 && errno == ENOMEM);
    CHECK(mremap(dest, p, p, MREMAP_MAYMOVE | MREMAP_FIXED, arena) == arena);
    CHECK(arena[0] == 42 && block[0] == 73);
    munmap(dest, 2 * p);
    munmap(arena, 4 * p);
    stack_t stack = {.ss_sp = malloc(32768), .ss_size = 32768}, old;
    replacement.ss_sp = malloc(32768);
    replacement.ss_size = 32768;
    if (!stack.ss_sp || !replacement.ss_sp)
        return 2;
    CHECK(sigaltstack(&stack, NULL) == 0);
    struct sigaction sa = {.sa_handler = handler, .sa_flags = SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    CHECK(raise(SIGUSR1) == 0);
    CHECK(disable_rc == -1 && disable_errno == EPERM);
    CHECK(replace_rc == -1 && replace_errno == EPERM);
    CHECK(query_flags == SS_ONSTACK);
    CHECK(sigaltstack(NULL, &old) == 0 && old.ss_sp == stack.ss_sp && old.ss_size == 32768 &&
          old.ss_flags == 0);
    // Use the raw Linux syscall to exercise pointer aliasing; libc's
    // sigaltstack prototype declares its arguments restrict-qualified.
    stack_t swap = replacement;
    CHECK(syscall(SYS_sigaltstack, &swap, &swap) == 0);
    CHECK(swap.ss_sp == stack.ss_sp);
    CHECK(sigaltstack(NULL, &old) == 0 && old.ss_sp == replacement.ss_sp);
    stack_t off = {.ss_flags = SS_DISABLE};
    CHECK(sigaltstack(&off, NULL) == 0);
    free(stack.ss_sp);
    free(replacement.ss_sp);
    printf("vm_signal_contracts: %d checks\n", checks);
    puts(failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
