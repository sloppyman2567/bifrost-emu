#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
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
    // SCM_RIGHTS contains descriptor numbers, not opaque bytes. Deliberately
    // separate the guest descriptor range from the emulator's host descriptors.
    int memory = memfd_create("socket-rights", MFD_ALLOW_SEALING);
    CHECK(memory >= 0 && ftruncate(memory, 4096) == 0);
    CHECK(pwrite(memory, "shared", 6, 0) == 6);
    int high = fcntl(memory, F_DUPFD, 200);
    CHECK(high >= 200);
    struct stat expected; CHECK(fstat(memory, &expected) == 0);
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(2 * sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct iovec byte = {first, 1};
    struct msghdr rights = {.msg_iov = &byte, .msg_iovlen = 1,
                           .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&rights);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(2 * sizeof(int));
    int descriptors[] = {high, high}; memcpy(CMSG_DATA(header), descriptors, sizeof(descriptors));
    CHECK(sendmsg(sockets[0], &rights, 0) == 1);
    close(high); close(memory); // queued rights own their references
    memset(&control, 0, sizeof(control));
    CHECK(recvmsg(sockets[1], &rights, MSG_CMSG_CLOEXEC) == 1);
    CHECK(!(rights.msg_flags & MSG_CTRUNC));
    header = CMSG_FIRSTHDR(&rights);
    CHECK(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
    CHECK(header->cmsg_len == CMSG_LEN(sizeof(descriptors)));
    memcpy(descriptors, CMSG_DATA(header), sizeof(descriptors));
    CHECK(descriptors[0] != descriptors[1]);
    for (int i = 0; i < 2; i++) {
        struct stat actual; CHECK(fstat(descriptors[i], &actual) == 0);
        CHECK(actual.st_dev == expected.st_dev && actual.st_ino == expected.st_ino);
        CHECK(fcntl(descriptors[i], F_GETFD) & FD_CLOEXEC);
        CHECK(fcntl(descriptors[i], F_GET_SEALS) == 0);
    }
    void *mapping = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, descriptors[0], 0);
    CHECK(mapping != MAP_FAILED && !memcmp(mapping, "shared", 6));
    memcpy(mapping, "mapped", 6);
    CHECK(pread(descriptors[1], first, 4, 0) == 4 && !memcmp(first, "mapp", 4));
    CHECK(munmap(mapping, 4096) == 0);
    CHECK(fcntl(descriptors[0], F_ADD_SEALS, F_SEAL_WRITE) == 0);
    CHECK(fcntl(descriptors[1], F_GET_SEALS) & F_SEAL_WRITE);
    close(descriptors[0]); close(descriptors[1]);
    rights.msg_controllen = sizeof(control.bytes);
    header = CMSG_FIRSTHDR(&rights);
    header->cmsg_len = CMSG_LEN(sizeof(int));
    int invalid = -1; memcpy(CMSG_DATA(header), &invalid, sizeof(invalid));
    errno = 0; CHECK(sendmsg(sockets[0], &rights, 0) == -1 && errno == EBADF);
    close(sockets[0]); close(sockets[1]);
    puts("socket_msg_layout: checks passed");
}
