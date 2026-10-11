// test_dyn_malloc.c — exercise glibc malloc with write() output.
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void putstr(const char *s) {
    write(1, s, strlen(s));
}

int main() {
    putstr("start\n");
    for (int i = 0; i < 5; i++) {
        char *p = malloc(64 + i * 32);
        if (!p) { putstr("malloc failed\n"); return 1; }
        memset(p, 'A' + i, 64 + i * 32);
        p[0] = '0' + i;
        p[1] = ':';
        p[2] = ' ';
        p[63 + i * 32] = '\n';
        write(1, p, 64 + i * 32);
        free(p);
    }
    putstr("done\n");
    return 0;
}
