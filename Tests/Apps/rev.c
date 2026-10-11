/* rev.c — reverse lines, like Unix `rev` */
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char buf[4096];
    while (fgets(buf, sizeof(buf), stdin)) {
        size_t n = strlen(buf);
        if (n && buf[n-1] == '\n') {
            buf[n-1] = 0;
            n--;
        }
        for (size_t i = 0; i < n / 2; i++) {
            char t = buf[i];
            buf[i] = buf[n-1-i];
            buf[n-1-i] = t;
        }
        fputs(buf, stdout);
        fputc('\n', stdout);
    }
    return 0;
}
