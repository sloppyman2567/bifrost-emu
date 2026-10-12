// test_sandbox.c — BIFROST_ROOT sandbox confinement regression tests.
//
// Fixtures are provisioned by Scripts/Testing/run_tests.sh (sandbox section):
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
// /proc PID denial; in-root mkdir/unlink/rename; chdir confinement;
// stat/access/readlink/chmod/chown/utimens/truncate/renameat2/linkat
// through an escaping link fail while the same calls work in the root.
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
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif

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
    // 7. /proc/<other-pid>/* denied under root (host proc-table leak);
    // the guest's own pid is /proc/self, as on Linux.
    {
        errno = 0;
        int f = open("/proc/2/maps", O_RDONLY);
        int e = errno;
        CHECK(f < 0, "/proc/2/maps denied under root");
        CHECK(e == ENOENT, "/proc denial is ENOENT");
        if (f >= 0) close(f);
        char own[64];
        snprintf(own, sizeof(own), "/proc/%d/maps", getpid());
        f = open(own, O_RDONLY);
        CHECK(f >= 0, "/proc/<getpid()>/maps is the guest's own");
        if (f >= 0) close(f);
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe));
        CHECK(n > 0 && exe[0] == '/', "proc exe path is absolute inside root");
        char clipped[2] = {'?', '?'};
        CHECK(readlink("/proc/self/exe", clipped, 1) == 1 &&
              clipped[0] == '/' && clipped[1] == '?', "proc readlink truncates without NUL");
        errno = 0;
        CHECK(syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", clipped, 0) == -1 && errno == EINVAL,
              "proc readlink zero size returns EINVAL");
        // ... while /proc/self/* keeps working.
        f = open("/proc/self/cmdline", O_RDONLY);
        CHECK(f >= 0, "/proc/self/cmdline still works");
        if (f >= 0) close(f);
    }
    // 7b. Guest descriptor readlinks.
    // Descriptor links must use guest identities, including dup and dirfd
    // forms. A high guest fd deliberately differs from the wrapped host fd.
    {
        int f = open("/sub/real.txt", O_RDONLY);
        int high = f >= 0 ? fcntl(f, F_DUPFD, 300) : -1;
        CHECK(high >= 300, "duplicate descriptor for proc fd translation");
        if (high >= 0) {
            char path[80], target[4096];
            snprintf(path, sizeof(path), "/proc/self/fd/%d", high);
            ssize_t n = readlink(path, target, sizeof(target));
            CHECK(n == 13 && !memcmp(target, "/sub/real.txt", 13), "proc fd reports guest-root path");
            char clipped[2] = {'?', '?'};
            CHECK(readlink(path, clipped, 1) == 1 && clipped[0] == '/' && clipped[1] == '?',
                  "proc fd truncation has no NUL");
            int directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
            snprintf(path, sizeof(path), "%d", high);
            n = readlinkat(directory, path, target, sizeof(target));
            CHECK(n == 13 && !memcmp(target, "/sub/real.txt", 13), "proc fd relative directory lookup");
            if (directory >= 0) close(directory);
            snprintf(path, sizeof(path), "/proc/1/fd/%d", high);
            CHECK(readlink(path, target, sizeof(target)) == 13, "guest process fd alias");
            close(high);
            errno = 0;
            CHECK(readlink(path, target, sizeof(target)) < 0 && errno == ENOENT,
                  "closed guest descriptor is absent");
        }
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
        int parentfd = open("../sub/real.txt", O_RDONLY);
        CHECK(parentfd >= 0, "relative parent traversal inside guest root works");
        if (parentfd >= 0) close(parentfd);
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

    // 14. Metadata/attribute syscalls resolve confined too. Through the
    // escaping dir symlink each must fail (as under chroot); run_tests.sh
    // also checks afterwards that the outside canary's content and mode
    // are unchanged. The same calls inside the root keep working.
    {
        struct stat st;
        struct timespec ts[2] = {{1, 0}, {1, 0}};
        char buf[256];
        const char* out = "/sub/outlink/secret";
#define ESCAPE_FAILS(expr, msg) do { errno = 0; long r_ = (long)(expr); \
        CHECK(r_ < 0 && errno == ENOENT, msg); } while (0)
        ESCAPE_FAILS(stat(out, &st), "stat through escape fails");
        ESCAPE_FAILS(lstat(out, &st), "lstat through escape fails");
        ESCAPE_FAILS(access(out, F_OK), "access through escape fails");
        ESCAPE_FAILS(syscall(SYS_faccessat2, AT_FDCWD, out, F_OK, 0), "faccessat2 through escape fails");
        ESCAPE_FAILS(readlink("/sub/outlink/x", buf, sizeof(buf)), "readlink through escape fails");
        ESCAPE_FAILS(chmod(out, 0600), "chmod through escape fails");
        ESCAPE_FAILS(chown(out, (uid_t)-1, (gid_t)-1), "chown through escape fails");
        ESCAPE_FAILS(utimensat(AT_FDCWD, out, ts, 0), "utimensat through escape fails");
        ESCAPE_FAILS(truncate(out, 0), "truncate through escape fails");
        ESCAPE_FAILS(truncate("/sub/evil", 0), "truncate via escaping file link fails");
        ESCAPE_FAILS(syscall(SYS_renameat2, AT_FDCWD, "/sub/real.txt", AT_FDCWD,
                             "/sub/outlink/moved.txt", 0), "renameat2 through escape fails");
        ESCAPE_FAILS(linkat(AT_FDCWD, "/sub/evil", AT_FDCWD, "/sub/hard-evil",
                            AT_SYMLINK_FOLLOW), "linkat following escaping link fails");
#undef ESCAPE_FAILS
        CHECK(stat("/sub/real.txt", &st) == 0 && st.st_size == 6, "stat in root works");
        CHECK(lstat("/sub/evil", &st) == 0 && S_ISLNK(st.st_mode), "lstat of in-root link works");
        ssize_t n = readlink("/sub/evil", buf, sizeof(buf));
        CHECK(n > 0 && memmem(buf, (size_t)n, "outside/secret", 14) != NULL,
              "readlink of in-root link works");
        CHECK(access("/sub/real.txt", R_OK) == 0, "access in root works");
        CHECK(chmod("/sub/real.txt", 0600) == 0 && stat("/sub/real.txt", &st) == 0 &&
              (st.st_mode & 0777) == 0600, "chmod in root works");
        chmod("/sub/real.txt", 0644);
        CHECK(utimensat(AT_FDCWD, "/sub/real.txt", ts, 0) == 0 &&
              stat("/sub/real.txt", &st) == 0 && st.st_mtim.tv_sec == 1, "utimensat in root works");
        CHECK(utimensat(AT_FDCWD, "/sub/evil", ts, AT_SYMLINK_NOFOLLOW) == 0,
              "lutimes on in-root link works");
        CHECK(chown("/sub/real.txt", (uid_t)-1, (gid_t)-1) == 0, "chown in root works");
        unlink("/sub/inlink");
        unlink("/sub/hard");
        CHECK(symlink("real.txt", "/sub/inlink") == 0 &&
              linkat(AT_FDCWD, "/sub/inlink", AT_FDCWD, "/sub/hard", AT_SYMLINK_FOLLOW) == 0,
              "linkat following in-root link works");
        struct stat hard;
        CHECK(stat("/sub/hard", &hard) == 0 && stat("/sub/real.txt", &st) == 0 &&
              hard.st_ino == st.st_ino, "followed hard link targets the in-root file");
        CHECK(syscall(SYS_renameat2, AT_FDCWD, "/sub/hard", AT_FDCWD, "/sub/hard2",
                      RENAME_NOREPLACE) == 0, "renameat2 in root works");
        CHECK(truncate("/sub/hard2", 2) == 0 && stat("/sub/real.txt", &st) == 0 &&
              st.st_size == 2, "truncate in root works");
        int f = open("/sub/real.txt", O_WRONLY | O_TRUNC);
        CHECK(f >= 0 && write(f, "hello\n", 6) == 6, "restore real.txt");
        if (f >= 0) close(f);
        unlink("/sub/hard2");
        unlink("/sub/inlink");
        // cwd- and dirfd-relative forms inside the root.
        CHECK(chdir("/sub") == 0, "chdir into sub for relative checks");
        CHECK(stat("real.txt", &st) == 0 && access("real.txt", R_OK) == 0,
              "cwd-relative stat/access in root works");
        CHECK(mkdir("reldir", 0755) == 0 && rmdir("reldir") == 0,
              "cwd-relative mkdir/rmdir in root works");
        chdir("/");
        int dfd = open("/sub", O_RDONLY | O_DIRECTORY);
        CHECK(dfd >= 0 && fchownat(dfd, "real.txt", (uid_t)-1, (gid_t)-1, 0) == 0 &&
              syscall(SYS_faccessat2, dfd, "real.txt", R_OK, 0) == 0,
              "dirfd-relative fchownat/faccessat2 in root work");
        if (dfd >= 0) close(dfd);
    }

    if (fails == 0) printf("checks passed: %d\n", checks);
    else printf("FAILURES: %d/%d\n", fails, checks);
    return fails != 0;
}
