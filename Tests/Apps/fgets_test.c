/* fgets_test.c — minimal test of fgets + printf */
#include <stdio.h>
#include <string.h>

int main(void) {
    char buf[256];
    fputs("input: ", stdout);
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        fputs("\nEOF\n", stdout);
        return 1;
    }
    fputs("got: [", stdout);
    fputs(buf, stdout);
    fputs("]\n", stdout);
    return 0;
}
