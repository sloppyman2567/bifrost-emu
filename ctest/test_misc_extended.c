// test_misc_extended.c — regression coverage for the compatibility
// syscalls added to src/syscalls/misc_extended.cpp.
//
// Two categories are checked:
//   - permissive no-ops that must report success (NUMA policy, ioprio,
//     settimeofday/adjtimex, vhangup/swapoff)
//   - genuinely unsupported facilities that must report a specific error
//     (-ENOSYS / -EPERM) so callers can pick a fallback
//
// Numbers are the asm-generic/unistd.h values AArch64 uses. musl's
// syscall() takes the raw number, so no libc wrappers are needed.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/timex.h>

#ifndef SYS_mbind
#define SYS_mbind 235
#endif
#ifndef SYS_set_mempolicy
#define SYS_set_mempolicy 237
#endif
#ifndef SYS_get_mempolicy
#define SYS_get_mempolicy 236
#endif
#ifndef SYS_move_pages
#define SYS_move_pages 239
#endif
#ifndef SYS_migrate_pages
#define SYS_migrate_pages 238
#endif
#ifndef SYS_ioprio_set
#define SYS_ioprio_set 30
#endif
#ifndef SYS_ioprio_get
#define SYS_ioprio_get 31
#endif
#ifndef SYS_settimeofday
#define SYS_settimeofday 170
#endif
#ifndef SYS_adjtimex
#define SYS_adjtimex 171
#endif
#ifndef SYS_vhangup
#define SYS_vhangup 58
#endif
#ifndef SYS_swapoff
#define SYS_swapoff 225
#endif
#ifndef SYS_fanotify_init
#define SYS_fanotify_init 262
#endif
#ifndef SYS_bpf
#define SYS_bpf 280
#endif
#ifndef SYS_userfaultfd
#define SYS_userfaultfd 282
#endif
#ifndef SYS_mq_unlink
#define SYS_mq_unlink 181
#endif
#ifndef SYS_quotactl
#define SYS_quotactl 60
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

static void test_numa(void) {
    unsigned long nodemask[4] = {~0UL, ~0UL, ~0UL, ~0UL};
    int policy = 123;
    long r;

    r = syscall(SYS_mbind, (void*)0x1000, 4096, 0, nodemask, 256, 0);
    CHECK(r == 0, "mbind accepted (no-op)");

    r = syscall(SYS_set_mempolicy, 0, nodemask, 256);
    CHECK(r == 0, "set_mempolicy accepted (no-op)");

    policy = 123;
    r = syscall(SYS_get_mempolicy, &policy, nodemask, 256, (void*)0, 0);
    CHECK(r == 0, "get_mempolicy succeeds");
    CHECK(policy == 0, "get_mempolicy reports MPOL_DEFAULT");
    CHECK(nodemask[0] == 0 && nodemask[1] == 0, "get_mempolicy zeroes nodemask");

    r = syscall(SYS_migrate_pages, 0, 256, nodemask, nodemask);
    CHECK(r == 0, "migrate_pages reports 0 unmoved");

    r = syscall(SYS_move_pages, 0, 0, (void**)0, (int*)0, (int*)0, 0);
    CHECK(r == 0, "move_pages reports 0 = all moved");
}

static void test_permissive(void) {
    long r;

    r = syscall(SYS_ioprio_set, 1, 0, 0);
    CHECK(r == 0, "ioprio_set succeeds");
    r = syscall(SYS_ioprio_get, 1, 0);
    CHECK(r >= 0, "ioprio_get returns a priority");

    struct timeval tv;
    tv.tv_sec = 1000000000;
    tv.tv_usec = 0;
    r = syscall(SYS_settimeofday, &tv, (void*)0);
    CHECK(r == 0, "settimeofday succeeds");

    struct timex tx;
    memset(&tx, 0xAA, sizeof(tx));
    tx.modes = 0;
    r = syscall(SYS_adjtimex, &tx);
    CHECK(r == 0, "adjtimex returns TIME_OK");
    CHECK(tx.modes == 0 && tx.offset == 0, "adjtimex clears the timex struct");

    r = syscall(SYS_vhangup);
    CHECK(r == 0, "vhangup succeeds");

    r = syscall(SYS_swapoff, (void*)0);
    CHECK(r == 0, "swapoff succeeds");
}

static void test_unsupported(void) {
    long r;
    errno = 0;
    r = syscall(SYS_fanotify_init, 0, 0);
    CHECK(r == -1 && errno == ENOSYS, "fanotify_init -> ENOSYS (262)");

    errno = 0;
    r = syscall(SYS_bpf, 0, (void*)0, 0);
    CHECK(r == -1 && errno == ENOSYS, "bpf -> ENOSYS");

    errno = 0;
    r = syscall(SYS_userfaultfd, 0);
    CHECK(r == -1 && errno == ENOSYS, "userfaultfd -> ENOSYS");

    errno = 0;
    r = syscall(SYS_mq_unlink, "/nonexistent");
    CHECK(r == -1 && errno == ENOSYS, "mq_unlink -> ENOSYS");

    errno = 0;
    r = syscall(SYS_quotactl, 0, (void*)0, 0, (void*)0);
    CHECK(r == -1 && errno == ENOSYS, "quotactl -> ENOSYS");
}

int main(void) {
    test_numa();
    test_permissive();
    test_unsupported();
    if (failures == 0) {
        printf("test_misc_extended: ALL TESTS PASSED (%d checks)\n", checks);
        return 0;
    }
    printf("test_misc_extended: %d/%d FAILED\n", failures, checks);
    return 1;
}
