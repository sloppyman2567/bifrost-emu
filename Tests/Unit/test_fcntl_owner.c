// test_fcntl_owner.c — fcntl F_SETOWN/F_GETOWN/F_SETSIG/F_GETSIG/F_SETLEASE/F_GETLEASE.
//
// The owner and signal belong to the open file description, so a dup() shares them; the owner must be
// a live pid or process group (ESRCH otherwise, INT_MIN is EINVAL), and a group reads back negative.
// F_SETSIG accepts 0..64. Anti-tamper code (Umamusume's CrackProof) hands a forked helper the
// descriptors of /proc/<pid>/mem and a socket, then calls F_SETOWN(parent) on them; EINVAL there is
// taken as a hostile environment and the helper ends the app.
//
// F_SETLEASE works on regular files (EINVAL on pipes, sockets and directories; EAGAIN when removing a
// lease that is not held) and F_GETLEASE reads the lease back. Kernels before 6.15 also lease procfs
// files, so /proc/<pid>/mem takes a lease; the protector sets one on it and a refusal is the same
// "hostile environment". Newer kernels refuse (simple_nosetlease): run natively, that one check is
// tolerated; the emulator is run with the argument "emu" and implements the older, wider behaviour.
//
// ioctl(FIOCLEX/FIONCLEX, NULL) sets and clears close-on-exec on any descriptor, and a request a file
// does not implement is ENOTTY even with a NULL argument. CPython calls ioctl(fd, FIOCLEX, NULL) after
// every fopen and treats any other error as a failed open: an EFAULT made Ren'Py (Doki Doki Literature
// Club) report "can't open file 'main.py': [Errno 14] Bad address".
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s (errno %d)\n", what, errno); failures++; } } while (0)

static int strict_proc_lease;  // set with the "emu" argument

// `lease`: 0 = F_SETLEASE is refused (EINVAL), 1 = it works, 2 = it works only on kernels before 6.15.
static void check_fd(int fd, const char *name, pid_t owner, int lease) {
    char what[96];
    snprintf(what, sizeof what, "%s: fresh F_GETOWN is 0", name);
    CHECK(fcntl(fd, F_GETOWN) == 0, what);
    snprintf(what, sizeof what, "%s: F_SETOWN(live pid)", name);
    CHECK(fcntl(fd, F_SETOWN, owner) == 0, what);
    snprintf(what, sizeof what, "%s: F_GETOWN reads it back", name);
    CHECK(fcntl(fd, F_GETOWN) == owner, what);
    snprintf(what, sizeof what, "%s: F_SETSIG(SIGIO)", name);
    CHECK(fcntl(fd, F_SETSIG, 29) == 0, what);
    snprintf(what, sizeof what, "%s: F_GETSIG reads it back", name);
    CHECK(fcntl(fd, F_GETSIG) == 29, what);
    snprintf(what, sizeof what, "%s: F_GETLEASE is F_UNLCK before any lease", name);
    CHECK(fcntl(fd, F_GETLEASE) == F_UNLCK, what);
    errno = 0;
    int r = fcntl(fd, F_SETLEASE, F_WRLCK);
    snprintf(what, sizeof what, "%s: F_SETLEASE(F_WRLCK)", name);
    if (lease == 1 || (lease == 2 && strict_proc_lease)) {
        CHECK(r == 0, what);
        snprintf(what, sizeof what, "%s: F_GETLEASE reads the lease back", name);
        CHECK(fcntl(fd, F_GETLEASE) == F_WRLCK, what);
        snprintf(what, sizeof what, "%s: F_SETLEASE(F_UNLCK) releases it", name);
        CHECK(fcntl(fd, F_SETLEASE, F_UNLCK) == 0 && fcntl(fd, F_GETLEASE) == F_UNLCK, what);
    } else if (lease == 2) {
        CHECK(r == 0 || (r == -1 && errno == EINVAL), what);
    } else {
        CHECK(r == -1 && errno == EINVAL, what);
    }
}

int main(int argc, char **argv) {
    strict_proc_lease = argc > 1 && strcmp(argv[1], "emu") == 0;
    char path[64];
    pid_t parent = getpid();
    snprintf(path, sizeof path, "/proc/%d/mem", parent);
    int mem = open(path, O_RDONLY);
    int pipefd[2], sock[2];
    CHECK(mem >= 0, "open /proc/<pid>/mem");
    CHECK(pipe(pipefd) == 0, "pipe");
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sock) == 0, "socketpair");

    // The helper's view: a forked child names its parent as the owner of inherited descriptors.
    pid_t child = fork();
    if (child == 0) {
        check_fd(mem, "child mem", parent, 2);
        check_fd(pipefd[0], "child pipe", parent, 0);
        check_fd(sock[0], "child socket", parent, 0);
        fflush(stdout);
        _exit(failures ? 1 : 0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child, "waitpid");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child checks passed");

    // Validation, group owners, 0 clearing the owner, and sharing through dup.
    int fd = pipefd[1];
    errno = 0;
    CHECK(fcntl(fd, F_SETOWN, INT_MIN) == -1 && errno == EINVAL, "F_SETOWN(INT_MIN) is EINVAL");
    errno = 0;
    CHECK(fcntl(fd, F_SETOWN, 0x3ffffff0) == -1 && errno == ESRCH, "F_SETOWN(no such pid) is ESRCH");
    errno = 0;
    CHECK(fcntl(fd, F_SETSIG, 65) == -1 && errno == EINVAL, "F_SETSIG(65) is EINVAL");
    errno = 0;
    CHECK(fcntl(fd, F_SETSIG, -1) == -1 && errno == EINVAL, "F_SETSIG(-1) is EINVAL");
    CHECK(fcntl(fd, F_SETSIG, 64) == 0 && fcntl(fd, F_GETSIG) == 64, "F_SETSIG(64)");
    CHECK(fcntl(fd, F_SETSIG, 0) == 0 && fcntl(fd, F_GETSIG) == 0, "F_SETSIG(0) restores SIGIO");
    pid_t group = getpgrp();
    CHECK(fcntl(fd, F_SETOWN, -group) == 0, "F_SETOWN(-pgrp)");
    CHECK(fcntl(fd, F_GETOWN) == -group, "a group owner reads back negative");
    int copy = dup(fd);
    CHECK(fcntl(fd, F_SETOWN, parent) == 0, "F_SETOWN(pid) on the original");
    CHECK(fcntl(copy, F_GETOWN) == parent, "a dup() shares the owner");
    CHECK(fcntl(copy, F_SETSIG, 10) == 0 && fcntl(fd, F_GETSIG) == 10, "a dup() shares the signal");
    CHECK(fcntl(fd, F_SETOWN, 0) == 0 && fcntl(fd, F_GETOWN) == 0, "F_SETOWN(0) clears the owner");
    errno = 0;
    CHECK(fcntl(-1, F_GETOWN) == -1 && errno == EBADF, "F_GETOWN on a bad fd is EBADF");

    // Leases on an ordinary file: EAGAIN to release none, a read lease needs a read-only open, a lease is
    // read back, and an invalid type or a directory is EINVAL.
    char tmp[] = "/tmp/test_fcntl_owner.XXXXXX";
    int wr = mkstemp(tmp);
    CHECK(wr >= 0, "mkstemp");
    close(wr);  // a lease conflicts with any other opener; the emulator sees only this description's mode
    int rd = open(tmp, O_RDONLY);
    CHECK(rd >= 0, "open read-only");
    errno = 0;
    CHECK(fcntl(rd, F_SETLEASE, F_UNLCK) == -1 && errno == EAGAIN, "releasing a lease that is not held is EAGAIN");
    errno = 0;
    CHECK(fcntl(rd, F_SETLEASE, 7) == -1 && errno == EINVAL, "an invalid lease type is EINVAL");
    CHECK(fcntl(rd, F_SETLEASE, F_RDLCK) == 0 && fcntl(rd, F_GETLEASE) == F_RDLCK, "a read lease on a read-only file");
    CHECK(fcntl(rd, F_SETLEASE, F_UNLCK) == 0 && fcntl(rd, F_GETLEASE) == F_UNLCK, "releasing it");
    check_fd(rd, "regular file", parent, 1);
    close(rd);
    unlink(tmp);
    int dir = open("/", O_RDONLY | O_DIRECTORY);
    errno = 0;
    CHECK(dir >= 0 && fcntl(dir, F_SETLEASE, F_RDLCK) == -1 && errno == EINVAL, "a directory takes no lease");
    close(dir);

    // Close-on-exec through ioctl, on a regular file and a pipe; an unknown argument-free request is ENOTTY.
    char cx[] = "/tmp/test_fcntl_owner_cx.XXXXXX";
    int file = mkstemp(cx);
    int pipe_fds[2];
    CHECK(file >= 0 && pipe(pipe_fds) == 0, "a file and a pipe for FIOCLEX");
    int targets[] = {file, pipe_fds[0]};
    for (int i = 0; i < 2; i++) {
        int t = targets[i];
        CHECK(ioctl(t, FIOCLEX, NULL) == 0 && (fcntl(t, F_GETFD) & FD_CLOEXEC), "FIOCLEX sets close-on-exec");
        CHECK(ioctl(t, FIONCLEX, NULL) == 0 && !(fcntl(t, F_GETFD) & FD_CLOEXEC), "FIONCLEX clears it");
    }
    errno = 0;
    CHECK(ioctl(file, 0x5490, NULL) == -1 && errno == ENOTTY, "an unknown request on a file is ENOTTY, not EFAULT");
    errno = 0;
    CHECK(ioctl(-1, FIOCLEX, NULL) == -1 && errno == EBADF, "FIOCLEX on a bad fd is EBADF");
    close(file);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    unlink(cx);

    if (failures) { printf("test_fcntl_owner: %d FAILED\n", failures); return 1; }
    printf("test_fcntl_owner: ALL PASS\n");
    return 0;
}
