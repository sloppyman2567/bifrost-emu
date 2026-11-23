// test_syscall_trace_names.c — makes the socket syscalls whose names BIFROST_SYSCALL_TRACE once had under the wrong numbers
// (sendto, recvfrom, sendmsg, recvmsg were 40, 45, 41, 43: mount, truncate, pivot_root, statfs on AArch64).
// Scripts/Testing/test_syscall_names.sh runs it under the trace and checks the lines.
//
// Plain C: build it natively as well to compare with the host kernel.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failures++; } } while (0)

int main(void) {
    int fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0, "socketpair");
    char buffer[8];
    CHECK(sendto(fds[0], "a", 1, 0, NULL, 0) == 1, "sendto");
    CHECK(recvfrom(fds[1], buffer, sizeof buffer, 0, NULL, NULL) == 1 && buffer[0] == 'a', "recvfrom");
    struct iovec iov = {"b", 1};
    struct msghdr out = {0};
    out.msg_iov = &iov;
    out.msg_iovlen = 1;
    CHECK(sendmsg(fds[0], &out, 0) == 1, "sendmsg");
    struct iovec in = {buffer, sizeof buffer};
    struct msghdr received = {0};
    received.msg_iov = &in;
    received.msg_iovlen = 1;
    CHECK(recvmsg(fds[1], &received, 0) == 1 && buffer[0] == 'b', "recvmsg");
    printf(failures ? "test_syscall_trace_names: %d FAILED\n" : "test_syscall_trace_names: ALL PASS\n", failures);
    return failures != 0;
}
