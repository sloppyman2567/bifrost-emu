// POSIX record locks: native Linux oracle and guest pointer marshalling.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main(void) {
    char path[] = "/tmp/bifrost_lock_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create lock file");
    if (fd < 0) return 1;
    unlink(path);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1};
    check(fcntl(fd, F_SETLK, &lock) == 0, "set record lock from guest buffer");
    lock.l_type = F_UNLCK;
    check(fcntl(fd, F_SETLKW, &lock) == 0, "unlock through blocking command");
    lock.l_type = F_WRLCK;
    check(fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type == F_UNLCK,
          "GETLK writes unlocked result back");
    void *unmapped = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (unmapped != MAP_FAILED) {
        munmap(unmapped, 4096);
        errno = 0;
        check(fcntl(fd, F_SETLK, unmapped) == -1 && errno == EFAULT,
              "unmapped lock buffer returns EFAULT");
    } else check(0, "prepare unmapped lock buffer");
    struct flock *readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (readonly != MAP_FAILED) {
        *readonly = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1};
        mprotect(readonly, 4096, PROT_READ);
        check(fcntl(fd, F_SETLK, readonly) == 0, "SETLK only reads its buffer");
        errno = 0;
        check(fcntl(fd, F_GETLK, readonly) == -1 && errno == EFAULT,
              "GETLK needs a writable buffer");
        munmap(readonly, 4096);
        lock.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &lock);
    } else check(0, "map read-only lock buffer");

    int ready[2], release[2];
    if (pipe(ready) || pipe(release)) { close(fd); return 1; }
    // Exercise both 64-bit offsets and a signed start relative to EOF.
    const off_t start = ((off_t)1 << 33) + 17;
    check(ftruncate(fd, start + 4096) == 0, "prepare sparse file above 4 GiB");
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]); close(release[1]);
        struct flock held = {.l_type = F_WRLCK, .l_whence = SEEK_END,
                             .l_start = -4096, .l_len = 4096};
        char ok = fcntl(fd, F_SETLK, &held) == 0;
        write(ready[1], &ok, 1);
        read(release[0], &ok, 1);
        usleep(100000);
        held.l_type = F_UNLCK;
        int status = fcntl(fd, F_SETLK, &held);
        close(fd);
        _exit(status == 0 ? 0 : 1);
    }
    check(child > 0, "fork lock owner");
    close(ready[1]); close(release[0]);
    if (child > 0) {
        char ok = 0;
        check(read(ready[0], &ok, 1) == 1 && ok, "child acquires record lock");
        lock = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET,
                              .l_start = start, .l_len = 4096};
        check(fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type == F_WRLCK
                && lock.l_whence == SEEK_SET && lock.l_start == start
                && lock.l_len == 4096 && lock.l_pid == child,
              "GETLK copies conflict range and child PID back");
        lock.l_type = F_WRLCK;
        errno = 0;
        check(fcntl(fd, F_SETLK, &lock) == -1 && (errno == EACCES || errno == EAGAIN),
              "nonblocking lock preserves host conflict error");
        write(release[1], "x", 1);
        check(fcntl(fd, F_SETLKW, &lock) == 0, "blocking lock waits for child unlock");
        int status = 0;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child unlock completes");
    }
    close(ready[0]); close(release[1]); close(fd);
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    puts("ALL PASS");
    return 0;
}
