// test_dyn_full.c — comprehensive glibc dynamic linking test.
// Exercises printf with various format specs, malloc, strings, stdio,
// exit handlers, and environment. This catches subtle dynamic linking
// bugs that simple hello-world programs miss.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static void cleanup_fn(void) {
    write(1, "atexit ran\n", 11);
}

int main(int argc, char **argv) {
    atexit(cleanup_fn);

    // printf with various format specifiers
    printf("int: %d\n", 42);
    printf("hex: 0x%x\n", 0xdeadbeef);
    printf("string: %s\n", "hello");
    printf("char: %c\n", 'A');
    printf("float: %.2f\n", 3.14);
    printf("long: %ld\n", 1234567890L);
    printf("multi: %d %s %c %x\n", 1, "two", '3', 0xff);
    printf("padded: %5d|%-5d|\n", 42, 42);
    printf("zero-pad: %05d\n", 42);

    // malloc + free
    char *p = malloc(100);
    if (!p) { printf("malloc failed\n"); return 1; }
    strcpy(p, "malloc works");
    printf("malloc: %s\n", p);
    free(p);

    // multiple allocations
    int *arr = malloc(10 * sizeof(int));
    for (int i = 0; i < 10; i++) arr[i] = i * i;
    printf("arr[5]=%d\n", arr[5]);
    free(arr);

    // string functions
    const char *s = "hello world";
    printf("strlen: %zu\n", strlen(s));
    printf("strchr: %s\n", strchr(s, 'w'));

    // fprintf to stderr
    fprintf(stderr, "stderr ok\n");

    // fputs
    fputs("fputs ok\n", stdout);

    // puts (adds newline)
    puts("puts ok");

    // fflush
    fflush(stdout);

    // errno
    fopen("/nonexistent", "r");
    printf("errno after fopen fail: %d\n", errno);
    perror("perror test");

    // time
    time_t t = time(NULL);
    printf("time: %ld\n", (long)t);

    write(1, "ALL PASS\n", 9);
    return 0;
}
