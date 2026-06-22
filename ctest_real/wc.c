/* wc.c — real-world-style word counter (like Unix `wc`) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static unsigned long lines = 0, words = 0, chars = 0, bytes = 0;
static int in_word = 0;

static void count_stream(FILE* f) {
    int c;
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        chars++;
        if (c == '\n') lines++;
        if (isspace(c)) {
            if (in_word) { words++; in_word = 0; }
        } else {
            in_word = 1;
        }
    }
    if (in_word) words++;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        count_stream(stdin);
        printf("  %lu  %lu  %lu\n", lines, words, bytes);
        return 0;
    }
    unsigned long tl = 0, tw = 0, tb = 0;
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "r");
        if (!f) { perror(argv[i]); continue; }
        lines = words = bytes = chars = 0; in_word = 0;
        count_stream(f);
        fclose(f);
        printf("  %lu  %lu  %lu  %s\n", lines, words, bytes, argv[i]);
        tl += lines; tw += words; tb += bytes;
    }
    if (argc > 2) {
        printf("  %lu  %lu  %lu  total\n", tl, tw, tb);
    }
    return 0;
}
