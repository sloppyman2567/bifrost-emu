/* head.c — first N lines of input (Unix `head`) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    long n = 10;
    int file_start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0 && argc > 2) {
        n = atol(argv[2]);
        file_start = 3;
    }
    if (n < 0) n = 0;

    if (file_start >= argc) {
        // stdin
        char buf[4096];
        long seen = 0;
        while (seen < n && fgets(buf, sizeof(buf), stdin)) {
            fputs(buf, stdout);
            if (strchr(buf, '\n')) seen++;
        }
        return 0;
    }

    int show_headers = (argc - file_start) > 1;
    for (int i = file_start; i < argc; i++) {
        FILE* f = fopen(argv[i], "r");
        if (!f) { perror(argv[i]); continue; }
        if (show_headers) {
            if (i > file_start) fputs("\n", stdout);
            printf("==> %s <==\n", argv[i]);
        }
        char buf[4096];
        long seen = 0;
        while (seen < n && fgets(buf, sizeof(buf), f)) {
            fputs(buf, stdout);
            if (strchr(buf, '\n')) seen++;
        }
        fclose(f);
    }
    return 0;
}
