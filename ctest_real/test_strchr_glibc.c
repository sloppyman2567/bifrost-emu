// test_strchr.c — test strchr/strlen/memcmp under glibc dynamic
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *s = "hello%world";
    char *p = strchr(s, '%');
    printf("strchr result: %s\n", p ? p : "NULL");
    printf("strlen: %zu\n", strlen(s));
    printf("memcmp: %d\n", memcmp("abc", "abc", 3));

    // Test simple printf with no format specifiers
    printf("no_format\n");
    // Test printf with %s
    printf("fmt=%s\n", "test");
    // Test printf with %d
    printf("num=%d\n", 42);
    // Test printf with %c
    printf("char=%c\n", 'X');
    printf("PASS\n");
    return 0;
}
