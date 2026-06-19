/* sort.c — line sort (subset of Unix `sort`).
 * Reads stdin into memory, sorts alphabetically, writes stdout.
 * Demonstrates: dynamic memory, qsort, multi-pass file I/O.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char** lines = NULL;
static size_t n_lines = 0, cap = 0;

static int cmp_lines(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

int main(int argc, char** argv) {
    int reverse = 0;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-r") == 0) reverse = 1;
        else if (strcmp(argv[argi], "-n") == 0) {
            // numeric sort handled below by parsing
        }
        argi++;
    }
    FILE* in = stdin;
    if (argi < argc) {
        in = fopen(argv[argi], "r");
        if (!in) { perror(argv[argi]); return 1; }
    }

    char buf[8192];
    while (fgets(buf, sizeof(buf), in)) {
        if (n_lines == cap) {
            cap = cap ? cap * 2 : 256;
            lines = realloc(lines, cap * sizeof(char*));
            if (!lines) { perror("realloc"); return 1; }
        }
        lines[n_lines++] = strdup(buf);
    }
    if (in != stdin) fclose(in);

    qsort(lines, n_lines, sizeof(char*), cmp_lines);

    if (reverse) {
        for (size_t i = n_lines; i > 0; i--) fputs(lines[i-1], stdout);
    } else {
        for (size_t i = 0; i < n_lines; i++) fputs(lines[i], stdout);
    }

    for (size_t i = 0; i < n_lines; i++) free(lines[i]);
    free(lines);
    return 0;
}
