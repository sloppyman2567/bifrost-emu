// Activity-manager command client for the managed session supervisor.
#include "bionic.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>

static char request[8192];
static size_t used;
static int append(const char *text) {
    size_t length = strlen(text);
    if (length >= sizeof(request) - used) return -1;
    memcpy(request + used, text, length);
    used += length;
    request[used] = 0;
    return 0;
}
static int quoted(const char *text) {
    static const char hex[] = "0123456789abcdef";
    if (append("\"")) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        char escaped[7] = {0};
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = (char)*p; }
        else if (*p < 32) {
            memcpy(escaped, "\\u00", 4); escaped[4] = hex[*p >> 4]; escaped[5] = hex[*p & 15];
        } else escaped[0] = (char)*p;
        if (append(escaped)) return -1;
    }
    return append("\"");
}
static int transfer(int fd, void *data, size_t length, int writing) {
    unsigned char *bytes = data;
    while (length) {
        ssize_t size = writing ? send(fd, bytes, length, MSG_NOSIGNAL) : recv(fd, bytes, length, 0);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) return -1;
        bytes += size; length -= (size_t)size;
    }
    return 0;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    const char *token = getenv("ASGARD_PROCESS_TOKEN");
    if (!token || strlen(token) != 64 || argc < 2 || argc > 65) {
        fprintf(stderr, "am: requires a managed session and an activity command\n"); return 2;
    }
    if (append("{\"operation\":\"am\",\"token\":") || quoted(token) || append(",\"argv\":[")) return 2;
    for (int i = 1; i < argc; ++i)
        if ((i != 1 && append(",")) || quoted(argv[i])) return 2;
    if (append("]}")) return 2;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("am: socket"); return 1; }
    struct timeval timeout = {.tv_sec = 8};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, "/dev/socket/asgard_process");
    uint32_t length = htonl((uint32_t)used);
    char response[8193];
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        transfer(fd, &length, sizeof(length), 1) || transfer(fd, request, used, 1) ||
        transfer(fd, &length, sizeof(length), 0) || ntohl(length) > 8192 || !ntohl(length) ||
        transfer(fd, response, ntohl(length), 0)) {
        fprintf(stderr, "am: activity manager unavailable\n"); close(fd); return 1;
    }
    close(fd);
    response[ntohl(length)] = 0;
    const char *value = strstr(response, "\"exit_code\"");
    if (!value || !(value = strchr(value, ':'))) {
        fprintf(stderr, "am: %s\n", response); return 1;
    }
    char *end;
    long code = strtol(value + 1, &end, 10);
    if (end == value + 1 || code < 0 || code > 255) return 1;
    return (int)code;
}
