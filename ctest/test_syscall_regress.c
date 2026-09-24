#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *name) {
    if (ok) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s (errno=%d: %s)\n", name, errno, strerror(errno));
        failures++;
    }
}

int main(void) {
    char cwd[4096];
    long cwd_len = syscall(SYS_getcwd, cwd, sizeof(cwd));
    check(cwd_len > 0 && (size_t)cwd_len == strlen(cwd) + 1,
          "getcwd length includes NUL");

    errno = 0;
    void *bad_fd_map = (void *)syscall(SYS_mmap, 0, 4096,
        PROT_READ, MAP_PRIVATE, -1, 0);
    check(bad_fd_map == MAP_FAILED && errno == EBADF,
          "file mmap rejects invalid fd");

    int file_fd = open("ctest/test_syscall_regress.c", O_RDONLY);
    check(file_fd >= 0, "open file for mmap offset check");
    if (file_fd >= 0) {
        errno = 0;
        void *unaligned_map = (void *)syscall(SYS_mmap, 0, 4096,
            PROT_READ, MAP_PRIVATE, file_fd, 1);
        check(unaligned_map == MAP_FAILED && errno == EINVAL,
              "file mmap rejects unaligned offset");
        close(file_fd);
    }

    long inotify_fd = syscall(SYS_inotify_init1, IN_NONBLOCK | IN_CLOEXEC);
    check(inotify_fd >= 0, "inotify returns guest fd");
    if (inotify_fd >= 0) {
        long watch = syscall(SYS_inotify_add_watch, (int)inotify_fd, ".",
                             IN_OPEN | IN_CLOSE);
        check(watch >= 0, "inotify add watch resolves guest fd");
        if (watch >= 0)
            check(syscall(SYS_inotify_rm_watch, (int)inotify_fd, (int)watch) == 0,
                  "inotify remove watch resolves guest fd");
        check(close((int)inotify_fd) == 0, "inotify guest close succeeds");
        errno = 0;
        check(fcntl((int)inotify_fd, F_GETFD) == -1 && errno == EBADF,
              "closed inotify guest fd is no longer open");
    }

    printf("%d checks failed\n", failures);
    return failures ? 1 : 0;
}
