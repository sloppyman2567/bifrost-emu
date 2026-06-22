/* tr.c — character translator (subset of Unix `tr`) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void tr_translate(const char* from, const char* to) {
    size_t fl = strlen(from), tl = strlen(to);
    if (tl > fl) tl = fl;  // silently truncate
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        int matched = -1;
        for (size_t i = 0; i < fl; i++) {
            if (from[i] == c) { matched = (int)i; break; }
        }
        if (matched >= 0 && matched < (int)tl) {
            fputc(to[matched], stdout);
        } else if (matched >= 0) {
            fputc(to[tl - 1], stdout);  // last char repeats
        } else {
            fputc(c, stdout);
        }
    }
}

static void tr_delete(const char* set) {
    size_t sl = strlen(set);
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        int in_set = 0;
        for (size_t i = 0; i < sl; i++) {
            if (set[i] == c) { in_set = 1; break; }
        }
        if (!in_set) fputc(c, stdout);
    }
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "-d") == 0) {
        tr_delete(argv[2]);
        return 0;
    }
    if (argc == 3) {
        tr_translate(argv[1], argv[2]);
        return 0;
    }
    fprintf(stderr, "usage: tr [-d SET | FROM TO]\n");
    return 1;
}
