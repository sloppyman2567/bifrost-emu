#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
_Static_assert(offsetof(struct msghdr, msg_flags) == 48, "ARM64 LP64 flags offset");
_Static_assert(sizeof(struct msghdr) == 56, "ARM64 LP64 message size");
#define CHECK(x) do { if (!(x)) { printf("socket message check failed: %s\n", #x); return 1; } } while (0)
int main(void) {
    int sockets[2]; CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0);
    unsigned char first[4], second[4];
    memset(first, 0xa5, sizeof(first)); memset(second, 0xa5, sizeof(second));
    struct iovec iov[] = {{first, sizeof(first)}, {second, sizeof(second)}};
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2, .msg_flags = -1};
    CHECK(send(sockets[0], "abcde", 5, 0) == 5);
    CHECK(recvmsg(sockets[1], &msg, 0) == 5);
    CHECK(!memcmp(first, "abcd", 4) && second[0] == 'e');
    CHECK(second[1] == 0xa5 && second[2] == 0xa5 && second[3] == 0xa5);
    CHECK(msg.msg_flags == 0 && msg.msg_controllen == 0);
    CHECK(send(sockets[0], "0123456789", 10, 0) == 10);
    msg.msg_flags = 0;
    CHECK(recvmsg(sockets[1], &msg, 0) == 8);
    CHECK((msg.msg_flags & MSG_TRUNC) && msg.msg_controllen == 0);
    CHECK(!memcmp(first, "0123", 4) && !memcmp(second, "4567", 4));
    close(sockets[0]); close(sockets[1]);
    puts("socket_msg_layout: checks passed");
}
