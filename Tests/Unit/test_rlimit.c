// test_rlimit.c — resource limits behave like Linux's (a guest used to see "unlimited" everywhere and
// setrlimit did nothing). Covers getrlimit/setrlimit/prlimit64 and RLIMIT_NOFILE enforcement.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("FAIL: %s (errno %d)\n", msg, errno); } } while (0)

int main(void) {
    struct rlimit rl, again;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != 0 && rl.rlim_cur <= rl.rlim_max,
          "RLIMIT_NOFILE is reported with cur <= max");
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur >= 1 << 20,
          "RLIMIT_STACK is a real size, not unlimited");
    errno = 0;
    CHECK(getrlimit(99, &rl) == -1 && errno == EINVAL, "an unknown resource is EINVAL");

    // prlimit64 agrees with getrlimit, and reports the old value while setting a new one.
    struct rlimit old, wanted;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0, "read the NOFILE limit");
    CHECK(syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, &old) == 0 && old.rlim_cur == rl.rlim_cur && old.rlim_max == rl.rlim_max,
          "prlimit64 reads the same limits");
    errno = 0;
    CHECK(syscall(SYS_prlimit64, 999999, RLIMIT_NOFILE, NULL, &old) == -1 && errno == ESRCH, "prlimit64 of another pid is ESRCH");

    // Lowering the soft limit takes effect and is enforced on descriptors.
    wanted.rlim_cur = 24;
    wanted.rlim_max = rl.rlim_max;
    CHECK(syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &wanted, &old) == 0 && old.rlim_cur == rl.rlim_cur,
          "prlimit64 sets a new limit and returns the old one");
    CHECK(getrlimit(RLIMIT_NOFILE, &again) == 0 && again.rlim_cur == 24 && again.rlim_max == rl.rlim_max,
          "getrlimit sees the new soft limit");
    int fds[64], opened = 0, last_errno = 0;
    for (int i = 0; i < 64; i++) {
        fds[i] = open("/dev/null", O_RDONLY);
        if (fds[i] < 0) { last_errno = errno; break; }
        opened++;
    }
    CHECK(last_errno == EMFILE && opened > 0 && opened < 24, "open fails with EMFILE once the limit is reached");
    for (int i = 0; i < opened; i++) close(fds[i]);
    int spare = dup(0);
    CHECK(spare >= 0 && spare < 24, "descriptors are available again after closing");
    if (spare >= 0) close(spare);

    // setrlimit validation.
    wanted.rlim_cur = 100; wanted.rlim_max = 50;
    errno = 0;
    CHECK(setrlimit(RLIMIT_NOFILE, &wanted) == -1 && errno == EINVAL, "cur > max is EINVAL");
    wanted.rlim_cur = 30; wanted.rlim_max = 30;
    CHECK(setrlimit(RLIMIT_NOFILE, &wanted) == 0 && getrlimit(RLIMIT_NOFILE, &again) == 0 &&
          again.rlim_cur == 30 && again.rlim_max == 30, "lowering the hard limit works");
    if (geteuid() != 0) {
        wanted.rlim_cur = 30; wanted.rlim_max = 40;
        errno = 0;
        CHECK(setrlimit(RLIMIT_NOFILE, &wanted) == -1 && errno == EPERM, "raising the hard limit needs privilege (EPERM)");
    }

    // Limits are inherited by a forked child.
    pid_t p = fork();
    if (p == 0) { struct rlimit c; _exit(getrlimit(RLIMIT_NOFILE, &c) == 0 && c.rlim_cur == 30 && c.rlim_max == 30 ? 0 : 1); }
    int st = 0;
    waitpid(p, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a fork child inherits the limits");

    if (fails == 0) printf("test_rlimit: ALL PASS (%d checks)\n", checks);
    else printf("test_rlimit: %d/%d FAILED\n", fails, checks);
    return fails != 0;
}
