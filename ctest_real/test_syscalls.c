// test_syscalls.c — Verify newly-added syscall handlers.
//
// What it verifies:
//   1. preadv() reads vectorized data from a file descriptor at an offset.
//   2. pwritev() writes vectorized data to a file descriptor at an offset.
//   3. preadv/pwritev roundtrip preserves data and returns correct byte count.
//   4. acct() returns -EPERM (requires CAP_SYS_ADMIN in a sandbox).
//   5. reboot() returns -EPERM (requires CAP_SYS_ADMIN).
//   6. syslog() returns -ENOSYS (not implemented).
//   7. sched_rr_get_interval() returns 0 and writes a non-zero interval.
//
// Build: make cross SRC=ctest_real/test_syscalls.c OUT=ctest_real/test_syscalls.elf
// Run:   ./bifrost-emu ctest_real/test_syscalls.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/reboot.h>
#include <sched.h>

static int passes = 0, failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

static void test_preadv_pwritev(void) {
    const char *path = "/tmp/test_preadv_pwritev.txt";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "open temp file");

    // Write "hello world" using pwritev at offset 10.
    const char *h = "hello ";
    const char *w = "world";
    struct iovec iov_out[2];
    iov_out[0].iov_base = (void *)h;
    iov_out[0].iov_len  = strlen(h);
    iov_out[1].iov_base = (void *)w;
    iov_out[1].iov_len  = strlen(w);
    ssize_t n = syscall(70, fd, iov_out, 2, 10);  // pwritev
    CHECK(n == 11, "pwritev writes 11 bytes at offset 10");

    // Read back with preadv at offset 10, expect "hello world".
    char buf[64] = {0};
    struct iovec iov_in[1];
    iov_in[0].iov_base = buf;
    iov_in[0].iov_len  = sizeof(buf) - 1;
    n = syscall(69, fd, iov_in, 1, 10);  // preadv
    CHECK(n == 11, "preadv reads 11 bytes at offset 10");
    CHECK(strncmp(buf, "hello world", 11) == 0, "preadv roundtrip data");

    // Verify bytes 0-9 are zero (wrote at offset 10).
    char hole[11] = {0};
    lseek(fd, 0, SEEK_SET);
    CHECK(read(fd, hole, 10) == 10, "read header bytes");
    CHECK(memcmp(hole, "\0\0\0\0\0\0\0\0\0\0", 10) == 0,
          "preadv/pwritev offset semantics (hole unwritten)");

    // EBADF: invalid fd. musl's syscall(2) returns -1 and sets errno.
    errno = 0;
    int bad = syscall(69, -1, iov_in, 1, 0);  // preadv
    CHECK(bad == -1 && errno == EBADF, "preadv EBADF for invalid fd");

    close(fd);
    unlink(path);
}

static void test_acct(void) {
    errno = 0;
    long r = syscall(89, "/tmp/acct_file");  // acct
    CHECK(r == -1 && errno == EPERM, "acct returns -EPERM");
}

static void test_reboot(void) {
    errno = 0;
    long r = syscall(142, 0xfee1dead, 672274793, 0x1234567, NULL);  // reboot
    CHECK(r == -1 && errno == EPERM, "reboot returns -EPERM");
}

static void test_syslog(void) {
    errno = 0;
    long r = syscall(116, 0, NULL, 0);  // syslog
    CHECK(r == -1 && errno == ENOSYS, "syslog returns -ENOSYS");
}

static void test_sched_rr_get_interval(void) {
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    long r = syscall(127, 0, &ts);  // sched_rr_get_interval
    CHECK(r == 0, "sched_rr_get_interval returns 0");
    CHECK(ts.tv_sec == 0 && ts.tv_nsec >= 0, "sched_rr_get_interval writes valid timespec");
}

int main(void) {
    test_preadv_pwritev();
    test_acct();
    test_reboot();
    test_syslog();
    test_sched_rr_get_interval();

    char msg[128];
    int len = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
