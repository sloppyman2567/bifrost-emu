#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

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

    int file_fd = open("Tests/Unit/test_syscall_regress.c", O_RDONLY);
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

    // Android DisplayEventReceiver depends on full 64-bit epoll cookies and
    // multiple naturally aligned AArch64 events, and removes fds with NULL.
#ifdef __aarch64__
    check(sizeof(struct epoll_event) == 16, "AArch64 epoll event is 16 bytes");
#endif
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int ready[2] = {eventfd(0, EFD_NONBLOCK), eventfd(0, EFD_NONBLOCK)};
    check(ep >= 0 && ready[0] >= 0 && ready[1] >= 0, "create epoll inputs");
    uint64_t cookies[2] = {UINT64_C(0x123456789abcdef0), UINT64_C(0xfedcba9876543210)};
    for (int i = 0; i < 2; i++) {
        struct epoll_event ev;
        memset(&ev, 0xa5, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.u64 = cookies[i];
        check(epoll_ctl(ep, EPOLL_CTL_ADD, ready[i], &ev) == 0, "epoll add aligned cookie");
    }
    for (int variant = 0; variant < 2; variant++) {
#ifndef SYS_epoll_pwait2
        if (variant) break;
#endif
        uint64_t one = 1;
        for (int i = 0; i < 2; i++) check(write(ready[i], &one, sizeof(one)) == sizeof(one), "ready epoll input");
        struct epoll_event events[2];
        memset(events, 0, sizeof(events));
        int count;
        if (!variant) count = epoll_pwait(ep, events, 2, 0, NULL);
#ifdef SYS_epoll_pwait2
        else {
            struct timespec zero = {0, 0};
            count = syscall(SYS_epoll_pwait2, ep, events, 2, &zero, NULL, 8);
            if (count == -1 && errno == ENOSYS) {
                printf("SKIP host kernel has no epoll_pwait2\n");
                break;
            }
        }
#endif
        check(count == 2 && (events[0].events & EPOLLIN) && (events[1].events & EPOLLIN), "epoll returns two aligned events");
        check((events[0].data.u64 == cookies[0] && events[1].data.u64 == cookies[1]) ||
              (events[0].data.u64 == cookies[1] && events[1].data.u64 == cookies[0]), "epoll preserves both 64-bit cookies");
        for (int i = 0; i < 2; i++) check(read(ready[i], &one, sizeof(one)) == sizeof(one), "drain epoll input");
    }
    check(epoll_ctl(ep, EPOLL_CTL_DEL, ready[0], NULL) == 0, "epoll DEL ignores NULL event");
    check(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_DEL, ready[1], (void *)-1) == 0, "epoll DEL ignores inaccessible event");
    close(ready[0]); close(ready[1]); close(ep);

    printf("%d checks failed\n", failures);
    return failures ? 1 : 0;
}
