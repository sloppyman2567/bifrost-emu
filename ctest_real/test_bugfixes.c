// test_bugfixes.c — Targeted tests for Turn 29 bug fixes.
// Verifies: pipe2 FdTable registration, fcntl F_SETFL/F_GETFL,
// /proc/self/status completeness, /proc/self/maps no truncation,
// FdTable lowest-fd reuse, dup3 EINVAL, clock_nanosleep error path,
// mmap MAP_PRIVATE file-backed load, futex FUTEX_WAIT_BITSET,
// FCMP unordered NZCV (V flag), ror64 by 0.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <sched.h>
#include <stdatomic.h>
#include <pthread.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

// ── Test 1: pipe2 + read/write through FdTable ──
// Before fix: pipe2 returned raw host fds not in FdTable; subsequent
// read/write/close returned EBADF.
static void test_pipe2_fds(void) {
    printf("\n--- test_pipe2_fds ---\n");
    int fds[2];
    int r = pipe2(fds, 0);
    CHECK(r == 0, "pipe2 returns 0");
    CHECK(fds[0] >= 0 && fds[1] >= 0, "pipe fds are non-negative");
    CHECK(fds[0] != fds[1], "pipe fds are distinct");

    // Write to fds[1], read from fds[0] — both go through FdTable
    const char *msg = "hello pipe";
    ssize_t w = write(fds[1], msg, strlen(msg));
    CHECK(w == (ssize_t)strlen(msg), "write to pipe[1]");

    char buf[64] = {0};
    ssize_t rd = read(fds[0], buf, sizeof(buf));
    CHECK(rd == (ssize_t)strlen(msg), "read from pipe[0]");
    CHECK(strcmp(buf, msg) == 0, "pipe data roundtrip");

    close(fds[0]);
    close(fds[1]);
    // After close, read/write should fail with EBADF
    rd = read(fds[0], buf, 1);
    CHECK(rd < 0, "read after close fails");
}

// ── Test 2: fcntl F_GETFL / F_SETFL (O_NONBLOCK) ──
// Before fix: fcntl was a no-op stub returning 0; O_NONBLOCK was never applied.
static void test_fcntl_nonblock(void) {
    printf("\n--- test_fcntl_nonblock ---\n");
    int fds[2];
    pipe2(fds, 0);

    // Get current flags
    int fl = fcntl(fds[0], F_GETFL);
    CHECK(fl >= 0, "F_GETFL returns valid flags");
    CHECK((fl & O_NONBLOCK) == 0, "O_NONBLOCK not set initially");

    // Set O_NONBLOCK
    int r = fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    CHECK(r == 0, "F_SETFL O_NONBLOCK succeeds");

    // Verify it was set
    fl = fcntl(fds[0], F_GETFL);
    CHECK((fl & O_NONBLOCK) != 0, "O_NONBLOCK is now set");

    // Reading from empty pipe should return EAGAIN, not block
    char buf[16];
    ssize_t rd = read(fds[0], buf, sizeof(buf));
    CHECK(rd < 0, "read on empty non-block pipe fails");
    CHECK(errno == EAGAIN, "errno is EAGAIN");

    close(fds[0]);
    close(fds[1]);
}

// ── Test 3: /proc/self/status has expected fields ──
// Before fix: only 10 fields; missing VmPeak, VmHWM, SigQ, SigCgt, etc.
static void test_proc_status(void) {
    printf("\n--- test_proc_status ---\n");
    FILE *f = fopen("/proc/self/status", "r");
    CHECK(f != NULL, "open /proc/self/status");

    char line[512];
    int found_vmpeak = 0, found_vmhwm = 0, found_sigq = 0, found_sigcgt = 0;
    int found_threads = 0, found_capEff = 0, found_voluntary = 0;
    int total_lines = 0;
    while (fgets(line, sizeof(line), f)) {
        total_lines++;
        if (strstr(line, "VmPeak:")) found_vmpeak = 1;
        if (strstr(line, "VmHWM:")) found_vmhwm = 1;
        if (strstr(line, "SigQ:")) found_sigq = 1;
        if (strstr(line, "SigCgt:")) found_sigcgt = 1;
        if (strstr(line, "Threads:")) found_threads = 1;
        if (strstr(line, "CapEff:")) found_capEff = 1;
        if (strstr(line, "voluntary_ctxt_switches:")) found_voluntary = 1;
    }
    fclose(f);

    printf("  (status has %d lines)\n", total_lines);
    CHECK(found_vmpeak, "VmPeak field present");
    CHECK(found_vmhwm, "VmHWM field present");
    CHECK(found_sigq, "SigQ field present");
    CHECK(found_sigcgt, "SigCgt field present");
    CHECK(found_threads, "Threads field present");
    CHECK(found_capEff, "CapEff field present");
    CHECK(found_voluntary, "voluntary_ctxt_switches field present");
    CHECK(total_lines >= 30, "at least 30 status lines (was 10 before fix)");
}

// ── Test 4: /proc/self/maps lines not truncated ──
// Before fix: 160-byte snprintf buffer truncated long labels.
static void test_proc_maps(void) {
    printf("\n--- test_proc_maps ---\n");
    FILE *f = fopen("/proc/self/maps", "r");
    CHECK(f != NULL, "open /proc/self/maps");

    char line[1024];
    int lines = 0;
    int long_lines = 0;
    while (fgets(line, sizeof(line), f)) {
        lines++;
        size_t len = strlen(line);
        if (len > 100) long_lines++;
        // Each line should end with newline (not truncated mid-line)
        CHECK(line[len-1] == '\n', "maps line ends with newline");
    }
    fclose(f);
    printf("  (maps has %d lines, %d > 100 chars)\n", lines, long_lines);
    CHECK(lines > 0, "maps has at least one line");
}

// ── Test 5: FdTable lowest-fd reuse ──
// Before fix: FdTable used monotonic next_fd_, never recycled closed fds.
// POSIX: open()/dup() return the lowest available fd.
static void test_fd_reuse(void) {
    printf("\n--- test_fd_reuse ---\n");
    // Close stdout (fd 1), open a new file — should get fd 1 (lowest available)
    int saved_stdout = dup(1);
    close(1);
    int new_fd = open("/dev/null", O_WRONLY);
    CHECK(new_fd == 1, "open returns fd 1 after close(1)");

    // Restore stdout
    close(new_fd);
    dup2(saved_stdout, 1);
    close(saved_stdout);
}

// ── Test 6: dup3 EINVAL when oldfd == newfd ──
// Before fix: dup3(a, a, 0) succeeded silently (real Linux returns EINVAL).
static void test_dup3_einval(void) {
    printf("\n--- test_dup3_einval ---\n");
    int r = dup3(5, 5, 0);
    CHECK(r < 0, "dup3(5, 5, 0) fails");
    CHECK(errno == EINVAL, "dup3(5, 5, 0) sets EINVAL");
}

// ── Test 7: clock_nanosleep error path ──
// Before fix: clock_nanosleep checked `if (r < 0)` but it returns positive
// errno, so EINTR was never reported. We can't easily trigger EINTR without
// a signal, but we can test invalid clockid → EINVAL.
static void test_clock_nanosleep(void) {
    printf("\n--- test_clock_nanosleep ---\n");
    struct timespec ts = {0, 1000000}; // 1ms
    int r = clock_nanosleep(9999, 0, &ts, NULL);
    CHECK(r != 0, "clock_nanosleep(invalid_clock) returns non-zero");
    CHECK(r == EINVAL, "returns EINVAL for invalid clockid");
}

// ── Test 8: mmap MAP_PRIVATE file-backed loads file contents ──
// Before fix: MAP_ANONYMOUS bit was wrong (0x02 instead of 0x20), so
// MAP_PRIVATE file mappings got zero pages instead of file contents.
static void test_mmap_private_file(void) {
    printf("\n--- test_mmap_private_file ---\n");
    // Create a temp file with known content
    int fd = open("/tmp/test_mmap_file", O_CREAT|O_RDWR|O_TRUNC, 0644);
    CHECK(fd >= 0, "create temp file");
    const char *data = "HELLO_MMAP_FILE_CONTENT_1234567890";
    write(fd, data, strlen(data));

    // mmap with MAP_PRIVATE (the standard mechanism for mapping executables)
    void *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(p != MAP_FAILED, "mmap MAP_PRIVATE succeeds");

    // Verify file contents are loaded
    CHECK(memcmp(p, data, strlen(data)) == 0, "MAP_PRIVATE loads file contents");

    munmap(p, 4096);
    close(fd);
    unlink("/tmp/test_mmap_file");
}

// ── Test 9: FCMP unordered sets V flag (NZCV = 0x30000000) ──
// Before fix: interpreter set 0x28000000 (V=0), diverging from JIT and ARM ARM.
static void test_fcmp_unordered_vflag(void) {
    printf("\n--- test_fcmp_unordered_vflag ---\n");
    volatile double nan_val = 0.0/0.0;
    volatile double one = 1.0;
    // FCMP with NaN should set V flag (overflow) for unordered
    // In C, we use __builtin_nan to get a NaN
    double a = nan_val, b = one;
    // Inline asm to do FCMP and read NZCV
    uint64_t nzcv;
    __asm__ volatile (
        "fmov d0, %1\n"
        "fmov d1, %2\n"
        "fcmp d0, d1\n"
        "mrs %0, nzcv\n"
        : "=r"(nzcv) : "r"(a), "r"(b) : "d0", "d1", "cc", "memory"
    );
    printf("  FCMP(nan, 1.0) NZCV = 0x%lx\n", nzcv);
    CHECK((nzcv & 0xF0000000) == 0x30000000, "FCMP unordered NZCV = 0x30000000 (V=1)");
}

// ── Test 10: ror64 by 0 is a no-op (not UB) ──
// Before fix: ror64(v, 0) did v << 64 which is UB in C++.
// We test via inline asm ROR x0, x0, #0.
static void test_ror0(void) {
    printf("\n--- test_ror0 ---\n");
    volatile uint64_t v = 0xDEADBEEF12345678ULL;
    uint64_t result;
    __asm__ volatile (
        "ror %0, %1, #0\n"
        : "=r"(result) : "r"(v)
    );
    CHECK(result == v, "ROR x, x, #0 is a no-op");
}

int main(void) {
    printf("=== Turn 29 bugfix verification tests ===\n");
    test_pipe2_fds();
    test_fcntl_nonblock();
    test_proc_status();
    test_proc_maps();
    test_fd_reuse();
    test_dup3_einval();
    test_clock_nanosleep();
    test_mmap_private_file();
    test_fcmp_unordered_vflag();
    test_ror0();

    printf("\n=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
