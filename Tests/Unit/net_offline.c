#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
// Run with BIFROST_ANDROID_OFFLINE=1 and argument "offline": IP traffic beyond loopback must fail with
// ENETUNREACH. With argument "any" only the loopback and Unix checks run (they must hold either way).
static int failures;
#define CHECK(x) do { if (!(x)) { printf("net_offline check failed: %s (errno %d)\n", #x, errno); failures++; } } while (0)

static struct sockaddr_in v4(const char* text, int port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, text, &a.sin_addr);
    return a;
}

int main(int argc, char** argv) {
    int offline = argc > 1 && !strcmp(argv[1], "offline");
    // Loopback TCP and UDP, and a Unix pair, are never affected.
    int server = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in local = v4("127.0.0.1", 0);
    socklen_t length = sizeof local;
    CHECK(bind(server, (struct sockaddr*)&local, sizeof local) == 0 && listen(server, 1) == 0);
    CHECK(getsockname(server, (struct sockaddr*)&local, &length) == 0);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, (struct sockaddr*)&local, sizeof local) == 0);
    int udp = socket(AF_INET, SOCK_DGRAM, 0), sink = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sink_address = v4("127.0.0.1", 0);
    length = sizeof sink_address;
    CHECK(bind(sink, (struct sockaddr*)&sink_address, sizeof sink_address) == 0);
    CHECK(getsockname(sink, (struct sockaddr*)&sink_address, &length) == 0);
    CHECK(sendto(udp, "x", 1, 0, (struct sockaddr*)&sink_address, sizeof sink_address) == 1);
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && write(pair[0], "x", 1) == 1);
    if (!offline) return failures ? 1 : (puts("net_offline: loopback checks passed"), 0);

    // Everything else is unreachable: TCP connect, UDP sendto, sendmsg, sendmmsg, IPv6 and v4-mapped.
    struct sockaddr_in remote = v4("192.0.2.1", 9), dns = v4("8.8.8.8", 53);
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(tcp, (struct sockaddr*)&remote, sizeof remote) == -1 && errno == ENETUNREACH);
    CHECK(sendto(udp, "x", 1, 0, (struct sockaddr*)&dns, sizeof dns) == -1 && errno == ENETUNREACH);
    struct iovec iov = {"x", 1};
    struct msghdr message = {.msg_name = &dns, .msg_namelen = sizeof dns, .msg_iov = &iov, .msg_iovlen = 1};
    CHECK(sendmsg(udp, &message, 0) == -1 && errno == ENETUNREACH);
    struct sockaddr_in loop_target = sink_address;
    struct mmsghdr batch[3] = {{.msg_hdr = {.msg_name = &loop_target, .msg_namelen = sizeof loop_target, .msg_iov = &iov, .msg_iovlen = 1}},
                               {.msg_hdr = message}, {.msg_hdr = message}};
    CHECK(sendmmsg(udp, batch, 3, 0) == 1);  // the loopback message goes out, the first refused one stops the batch
    batch[0].msg_hdr = message;
    CHECK(sendmmsg(udp, batch, 1, 0) == -1 && errno == ENETUNREACH);
    int tcp6 = socket(AF_INET6, SOCK_STREAM, 0);
    if (tcp6 >= 0) {
        struct sockaddr_in6 far = {.sin6_family = AF_INET6, .sin6_port = htons(9)}, mapped = far;
        inet_pton(AF_INET6, "2001:db8::1", &far.sin6_addr);
        inet_pton(AF_INET6, "::ffff:8.8.8.8", &mapped.sin6_addr);
        CHECK(connect(tcp6, (struct sockaddr*)&far, sizeof far) == -1 && errno == ENETUNREACH);
        CHECK(connect(tcp6, (struct sockaddr*)&mapped, sizeof mapped) == -1 && errno == ENETUNREACH);
    }
    if (!failures) puts("net_offline: offline checks passed");
    return failures ? 1 : 0;
}
