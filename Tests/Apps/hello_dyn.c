// hello_dyn.c — minimal dynamically-linked test for glibc/musl.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    printf("Hello, dynamic world!\n");
    char *p = strdup("test");
    if (!p) { printf("strdup failed\n"); return 1; }
    printf("strdup: %s (len=%zu)\n", p, strlen(p));
    free(p);
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d]=%s\n", i, argv[i]);
    }
    return 0;
}
