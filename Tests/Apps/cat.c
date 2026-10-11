/* cat.c — concatenate files (Unix `cat`) */
#include <stdio.h>
#include <string.h>

static int cat_file(FILE* f) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (fwrite(buf, 1, n, stdout) != n) return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 1) return cat_file(stdin);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            cat_file(stdin);
            continue;
        }
        FILE* f = fopen(argv[i], "r");
        if (!f) { perror(argv[i]); continue; }
        cat_file(f);
        fclose(f);
    }
    return 0;
}
