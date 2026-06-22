/* yes.c — emit a string forever (Unix `yes`) */
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    const char* s = (argc > 1) ? argv[1] : "y";
    // buffer for throughput
    char buf[4096];
    size_t slen = strlen(s);
    if (slen + 2 > sizeof(buf)) {
        // pathologically long arg — just write directly
        while (1) { fputs(s, stdout); fputc('\n', stdout); }
    }
    size_t total = 0;
    while (total + slen + 1 < sizeof(buf)) {
        memcpy(buf + total, s, slen);
        total += slen;
        buf[total++] = '\n';
    }
    while (1) {
        fwrite(buf, 1, total, stdout);
        // periodically flush so the host can stop us with timeout
    }
    return 0;
}
