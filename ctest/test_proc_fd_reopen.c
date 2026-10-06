// test_proc_fd_reopen.c — open("/proc/self/fd/N") reopens the file behind
// the GUEST descriptor N, as on Linux. Bifrost used to pass the path to the
// host procfs, which named the emulator's own fd N: reopening a memfd
// returned ENXIO (the emulator's fd there was a socket) or another file.
// Also checks /proc/self/fd/<dirfd>/name, ENOENT/ENXIO, and that openat2
// resolves the same way and translates guest open flags (O_DIRECTORY).
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

struct how { uint64_t flags, mode, resolve; };

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int reopen(int fd, int flags) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    return open(path, flags);
}

static int open2(const char* path, int flags) {
    struct how how = {(uint64_t)flags, 0, 0};
    return (int)syscall(437 /* openat2 */, AT_FDCWD, path, &how, sizeof how);
}

static int reads(int fd, const char* want) {
    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    return n == (ssize_t)strlen(want) && memcmp(buf, want, n) == 0;
}

int main(void) {
    // memfd: the new descriptor has its own offset, starting at 0.
    int mfd = (int)syscall(SYS_memfd_create, "reopen", 0);
    check(mfd >= 0 && write(mfd, "hello", 5) == 5, "memfd created");
    int r = reopen(mfd, O_RDONLY);
    check(r >= 0, "reopen memfd");
    check(r >= 0 && reads(r, "hello"), "reopened memfd reads from offset 0");
    if (r >= 0) close(r);

    // A guest fd number that the emulator itself does not use.
    check(dup2(mfd, 100) == 100, "dup2 memfd to fd 100");
    r = reopen(100, O_RDONLY);
    check(r >= 0 && reads(r, "hello"), "reopen through guest fd 100");
    if (r >= 0) close(r);

    // Regular file: writes through the reopened fd reach the same file.
    char file[] = "/tmp/proc_fd_reopen_XXXXXX";
    int ffd = mkstemp(file);
    r = ffd >= 0 ? reopen(ffd, O_RDWR) : -1;
    check(r >= 0 && write(r, "abc", 3) == 3, "reopen regular file read-write");
    char buf[4] = {0};
    check(pread(ffd, buf, 3, 0) == 3 && memcmp(buf, "abc", 3) == 0,
          "write via reopened fd visible via original");
    if (r >= 0) close(r);

    // Through a directory descriptor's link.
    char dir[] = "/tmp/proc_fd_dir_XXXXXX";
    char inner[64];
    int dfd = mkdtemp(dir) ? open(dir, O_RDONLY | O_DIRECTORY) : -1;
    snprintf(inner, sizeof inner, "%s/name", dir);
    int w = open(inner, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    check(w >= 0 && write(w, "inside", 6) == 6, "file inside directory");
    if (w >= 0) close(w);
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d/name", dfd);
    r = open(path, O_RDONLY);
    check(r >= 0 && reads(r, "inside"), "open /proc/self/fd/<dirfd>/name");
    if (r >= 0) close(r);

    // Errors.
    errno = 0;
    check(reopen(999, O_RDONLY) < 0 && errno == ENOENT, "closed fd: ENOENT");
    int sv[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    errno = 0;
    check(reopen(sv[0], O_RDWR) < 0 && errno == ENXIO, "socket: ENXIO");

    // openat2 shares openat's resolution and flag translation.
    snprintf(path, sizeof path, "/proc/self/fd/%d", mfd);
    r = open2(path, O_RDONLY);
    if (r < 0 && errno == ENOSYS) {
        printf("SKIP openat2: ENOSYS\n");
    } else {
        check(r >= 0 && reads(r, "hello"), "openat2 reopens memfd");
        if (r >= 0) close(r);
        r = open2(dir, O_RDONLY | O_DIRECTORY);
        check(r >= 0, "openat2 O_DIRECTORY on a directory");
        if (r >= 0) close(r);
        errno = 0;
        check(open2(file, O_RDONLY | O_DIRECTORY) < 0 && errno == ENOTDIR,
              "openat2 O_DIRECTORY on a file: ENOTDIR");
    }

    unlink(inner);
    rmdir(dir);
    unlink(file);
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
