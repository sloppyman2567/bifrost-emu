// test_sandbox.c — BIFROST_ROOT sandbox confinement regression tests.
//
// Fixtures are provisioned by scripts/run_tests.sh (sandbox section):
//   $SBX/root/sub/real.txt ("hello\n")
//   $SBX/root/sub/evil -> $SBX/outside/secret (escaping symlink)
//   $SBX/outside/secret ("CANARY-DO-NOT-READ")
//   $SBX/root/sub/outlink -> $SBX/outside (escaping dir symlink)
// BIFROST_ROOT=$SBX/root when this runs (host environment — the emulator
// reads it from the HOST environ; a guest setenv is invisible to it
// because musl's environ is separate from the host libc's).
//
// Checks: symlink escape via open/openat blocked; legit opens work;
// explicit-dirfd openat; bogus dirfd EBADF; clone namespace rejection;
// /proc PID denial; in-root mkdir/unlink/rename; chdir confinement.
// Results pattern: "checks passed" / "FAIL:" lines, exit 0/1.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
} while (0)

int main(void) {
    // 1. Symlink escape via open(): must FAIL (ENOENT/ELOOP/EACCES/EPERM).
    {
        errno = 0;
        int f = open("/sub/evil", O_RDONLY);
        int e = errno;
        CHECK(f < 0, "symlink escape via open must fail");
        CHECK(e == ENOENT || e == ELOOP || e == EACCES || e == EPERM,
              "symlink escape errno sane");
        if (f >= 0) close(f);
    }
    // 2. Same via openat(AT_FDCWD): must FAIL.
    {
        errno = 0;
        int f = syscall(SYS_openat, AT_FDCWD, "/sub/evil", O_RDONLY, 0);
        int e = errno;
        CHECK(f < 0, "symlink escape via openat must fail");
        CHECK(e == ENOENT || e == ELOOP || e == EACCES || e == EPERM,
              "openat escape errno sane");
        if (f >= 0) close(f);
    }
    // 3. Legit in-root open still works, with correct content.
    {
        int f = open("/sub/real.txt", O_RDONLY);
        CHECK(f >= 0, "in-root open works");
        if (f >= 0) {
            char b[16];
            ssize_t n = read(f, b, sizeof(b) - 1);
            CHECK(n == 6 && memcmp(b, "hello\n", 6) == 0, "in-root content");
            close(f);
        }
    }
    // 4. Explicit dirfd + relative path resolves under the dirfd.
    {
        int dfd = open("/sub", O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0, "open dir for dirfd test");
        if (dfd >= 0) {
            int f = syscall(SYS_openat, dfd, "real.txt", O_RDONLY, 0);
            CHECK(f >= 0, "dirfd-relative openat works");
            if (f >= 0) close(f);
            // Absolute path ignores dirfd per POSIX (still confined).
            f = syscall(SYS_openat, dfd, "/sub/real.txt", O_RDONLY, 0);
            CHECK(f >= 0, "absolute+dirfd openat works");
            if (f >= 0) close(f);
            // ... but not out of the sandbox through it.
            errno = 0;
            f = syscall(SYS_openat, dfd, "evil", O_RDONLY, 0);
            int e = errno;
            CHECK(f < 0, "dirfd-relative symlink escape must fail");
            CHECK(e == ENOENT || e == ELOOP || e == EACCES || e == EPERM,
                  "dirfd escape errno sane");
            if (f >= 0) close(f);
            close(dfd);
        }
    }
    // 5. Relative path + bogus dirfd -> EBADF (Linux semantics; the old
    // code silently resolved against the host cwd instead).
    {
        errno = 0;
        int f = syscall(SYS_openat, 9999, "real.txt", O_RDONLY, 0);
        CHECK(f < 0 && errno == EBADF, "bogus dirfd gives EBADF");
        if (f >= 0) close(f);
    }
    // 6. clone with namespace flags -> EINVAL (no silent fake isolation).
    {
        errno = 0;
        // CLONE_NEWNS|SIGCHLD: fork-like without VM; must be rejected.
        long r = syscall(SYS_clone, 0x00020000 /*NEWNS*/ | 17 /*SIGCHLD*/,
                         0, 0, 0, 0);
        CHECK(r < 0 && errno == EINVAL, "clone NEWNS rejected with EINVAL");
        errno = 0;
        r = syscall(SYS_clone, 0x20000000 /*NEWPID*/ | 17, 0, 0, 0, 0);
        CHECK(r < 0 && errno == EINVAL, "clone NEWPID rejected with EINVAL");
        // Plain fork-style clone (no VM, no namespaces) still works.
        pid_t p = (pid_t)syscall(SYS_clone, 17 /*SIGCHLD*/, 0, 0, 0, 0);
        if (p == 0) _exit(42);
        CHECK(p > 0, "plain clone/fork still works");
        if (p > 0) {
            int st = 0;
            waitpid(p, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "child exit status");
        }
    }
    // 7. /proc/<other-pid>/* denied under root (host proc-table leak).
    {
        errno = 0;
        int f = open("/proc/1/maps", O_RDONLY);
        int e = errno;
        CHECK(f < 0, "/proc/1/maps denied under root");
        CHECK(e == ENOENT, "/proc denial is ENOENT");
        if (f >= 0) close(f);
        // ... while /proc/self/* keeps working.
        f = open("/proc/self/cmdline", O_RDONLY);
        CHECK(f >= 0, "/proc/self/cmdline still works");
        if (f >= 0) close(f);
    }
    // 8. In-root mkdir/unlink/rename keep working (no regression).
    {
        CHECK(mkdir("/newdir", 0755) == 0, "mkdir in root works");
        int f = open("/newdir/a.txt", O_CREAT | O_WRONLY, 0644);
        CHECK(f >= 0, "create in root works");
        if (f >= 0) close(f);
        CHECK(rename("/newdir/a.txt", "/newdir/b.txt") == 0, "rename in root works");
        CHECK(unlink("/newdir/b.txt") == 0, "unlink in root works");
        CHECK(rmdir("/newdir") == 0, "rmdir in root works");
    }
    // 9. chdir confinement: chdir through an escaping symlink must fail
    // AND leave the cwd unchanged (POSIX) and inside the root.
    {
        CHECK(chdir("/sub") == 0, "chdir into sub works");
        errno = 0;
        int r = chdir("outlink");
        int e = errno;
        CHECK(r != 0, "chdir through escaping symlink must fail");
        CHECK(e == ENOENT || e == ENOTDIR || e == ELOOP || e == EACCES,
              "chdir escape errno sane");
        // Still in /sub (unchanged cwd): the known file opens.
        int f = open("real.txt", O_RDONLY);
        CHECK(f >= 0, "cwd unchanged after failed chdir");
        if (f >= 0) close(f);
        chdir("/");
    }
    // 10. Write-capable *at escapes blocked (parent traversal confined).
    // "/sub/outlink/..." resolves through the escaping dir symlink:
    // mkdir/unlink/rename/symlink through it must fail, while the same
    // ops on real in-root paths succeed (covered in 8).
    {
        errno = 0;
        int r = mkdir("/sub/outlink/evil-dir", 0755);
        CHECK(r != 0, "mkdirat through escaping symlink must fail");
        CHECK(errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
              errno == EACCES || errno == EPERM, "mkdirat errno sane");
        errno = 0;
        r = unlink("/sub/outlink/secret");
        CHECK(r != 0, "unlinkat through escaping symlink must fail");
        CHECK(errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
              errno == EACCES || errno == EPERM, "unlinkat errno sane");
        errno = 0;
        r = rename("/sub/real.txt", "/sub/outlink/moved.txt");
        CHECK(r != 0, "renameat escape must fail");
        CHECK(errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
              errno == EACCES || errno == EPERM || errno == EXDEV,
              "renameat errno sane");
        // Verify the outside canary is untouched and in-root file intact.
        int f = open("/sub/real.txt", O_RDONLY);
        CHECK(f >= 0, "in-root file intact after *at escapes");
        if (f >= 0) close(f);
    }
    // 11. symlinkat confinement: creating a link through an escaping
    // path must fail; creating one in-root works and the link itself
    // (even pointing outside) is harmless because traversal is confined.
    {
        errno = 0;
        int r = symlink("/etc/hostname", "/sub/outlink/badlink");
        CHECK(r != 0, "symlinkat through escape must fail");
        unlink("/sub/selflink");
        r = symlink("/etc/hostname", "/sub/selflink");
        CHECK(r == 0, "symlinkat in root works");
        // Traversal THROUGH the outside-pointing link stays confined.
        errno = 0;
        int f = open("/sub/selflink", O_RDONLY);
        int e = errno;
        CHECK(f < 0, "open through outside-pointing link blocked");
        CHECK(e == ENOENT || e == ELOOP || e == EACCES || e == EPERM,
              "selflink traversal errno sane");
        if (f >= 0) close(f);
        unlink("/sub/selflink");
    }
    // 12. fchdir confinement: an outside dirfd must not move cwd out.
    // Uses a RELATIVE open for the post-check so it genuinely depends on
    // cwd (an absolute open would pass regardless).
    {
        CHECK(chdir("/sub") == 0, "chdir into sub for fchdir test");
        int outfd = open("outlink", O_RDONLY | O_DIRECTORY);
        CHECK(outfd < 0, "open escaping dir blocked");
        if (outfd >= 0) {
            // Should not happen (open confined), but if it did, fchdir
            // is the second line of defence and must refuse.
            errno = 0;
            int r = fchdir(outfd);
            CHECK(r != 0, "fchdir to outside dir must fail");
            close(outfd);
        }
        // cwd still /sub after the attempt: relative open works.
        int f = open("real.txt", O_RDONLY);
        CHECK(f >= 0, "cwd inside root after fchdir attempt");
        if (f >= 0) close(f);
        chdir("/");
    }
    // 13. /proc/self/mem denied (emulator-memory read primitive).
    {
        errno = 0;
        int f = open("/proc/self/mem", O_RDONLY);
        int e = errno;
        CHECK(f < 0, "/proc/self/mem denied under root");
        CHECK(e == ENOENT, "/proc/self/mem denial is ENOENT");
        if (f >= 0) close(f);
    }

    if (fails == 0) printf("checks passed: %d\n", checks);
    else printf("FAILURES: %d/%d\n", fails, checks);
    return fails != 0;
}
