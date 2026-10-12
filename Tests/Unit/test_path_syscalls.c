// test_path_syscalls.c — path syscall regressions outside the sandbox.
//
// Covers:
//   - chmod()/fchmodat: the raw syscall has three arguments; a garbage x3
//     must not be forwarded as flags (every chmod failed with EINVAL).
//   - cwd-relative mkdir, rmdir, unlink, rename, symlink and link resolve
//     against AT_FDCWD.
//   - dirfd-relative fchownat/faccessat2 translate the guest dirfd.
//   - statx(fd, "", AT_EMPTY_PATH) stats the descriptor itself.
// Works in a fresh directory under /tmp; reference: native/qemu-aarch64.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; \
    if (!(cond)) { fails++; printf("FAIL: %s (errno %d)\n", msg, errno); } } while (0)

int main(void) {
    char dir[] = "/tmp/bifrost-path-XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) { perror("setup"); return 1; }
    int f = open("file", O_CREAT | O_WRONLY, 0644);
    CHECK(f >= 0 && write(f, "data", 4) == 4, "create file");
    if (f >= 0) close(f);

    struct stat st;
    // Put a nonzero value in x3 the way a caller's leftover register would.
    errno = 0;
    CHECK(syscall(SYS_fchmodat, AT_FDCWD, "file", 0600, 0x5a5a) == 0, "fchmodat ignores x3");
    CHECK(chmod("file", 0640) == 0 && stat("file", &st) == 0 && (st.st_mode & 0777) == 0640,
          "chmod sets the mode");

    CHECK(mkdir("sub", 0755) == 0, "cwd-relative mkdir");
    CHECK(symlink("file", "link") == 0, "cwd-relative symlink");
    CHECK(link("file", "hard") == 0, "cwd-relative link");
    CHECK(rename("hard", "hard2") == 0, "cwd-relative rename");
    CHECK(unlink("hard2") == 0 && unlink("link") == 0, "cwd-relative unlink");

    int dfd = open(".", O_RDONLY | O_DIRECTORY);
    // Occupy low guest fds so the guest dirfd number differs from any
    // host fd that happens to be a directory.
    int spare[4];
    for (int i = 0; i < 4; ++i) spare[i] = open("file", O_RDONLY);
    int dfd2 = dup(dfd);
    CHECK(dfd2 >= 0 && fchownat(dfd2, "file", (uid_t)-1, (gid_t)-1, 0) == 0,
          "dirfd-relative fchownat");
    CHECK(syscall(SYS_faccessat2, dfd2, "file", R_OK, 0) == 0, "dirfd-relative faccessat2");
    errno = 0;
    CHECK(syscall(SYS_faccessat2, dfd2, "missing", F_OK, 0) == -1 && errno == ENOENT,
          "dirfd-relative faccessat2 reports ENOENT");
    for (int i = 0; i < 4; ++i) if (spare[i] >= 0) close(spare[i]);

    f = open("file", O_RDONLY);
    unsigned char stx[256];
    memset(stx, 0, sizeof(stx));
    CHECK(f >= 0 && syscall(SYS_statx, f, "", AT_EMPTY_PATH, 0x7ff, stx) == 0,
          "statx AT_EMPTY_PATH on a descriptor");
    unsigned long long size;
    memcpy(&size, stx + 0x28, sizeof(size));
    CHECK(size == 4, "statx AT_EMPTY_PATH reports the descriptor's size");
    if (f >= 0) close(f);

    CHECK(rmdir("sub") == 0, "cwd-relative rmdir");
    unlink("file");
    if (dfd2 >= 0) close(dfd2);
    if (dfd >= 0) close(dfd);
    chdir("/");
    rmdir(dir);

    if (fails == 0) printf("test_path_syscalls: ALL PASS (%d checks)\n", checks);
    else printf("test_path_syscalls: %d/%d FAILED\n", fails, checks);
    return fails != 0;
}
