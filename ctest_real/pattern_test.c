#include <stdio.h>
#include <string.h>

// Simple glob pattern matcher (like fnmatch)
int match(const char *pattern, const char *str) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*str) {
                if (match(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?') {
            if (!*str) return 0;
            pattern++;
            str++;
        } else {
            if (*pattern != *str) return 0;
            pattern++;
            str++;
        }
    }
    return !*str;
}

int main() {
    printf("Glob pattern matcher test\n");
    
    struct { const char *pat, *str; int expect; } tests[] = {
        {"hello", "hello", 1},
        {"hello", "world", 0},
        {"h*o", "hello", 1},
        {"h*o", "hao", 1},
        {"h*o", "hi", 0},
        {"*.c", "main.c", 1},
        {"*.c", "main.cpp", 0},
        {"*test*", "my_test_file.c", 1},
        {"?at", "cat", 1},
        {"?at", "at", 0},
        {"a?c", "abc", 1},
        {"*", "", 1},
        {"", "", 1},
        {"", "a", 0},
        {NULL, NULL, 0}
    };
    
    for (int i = 0; tests[i].pat; i++) {
        int result = match(tests[i].pat, tests[i].str);
        printf("  match(\"%s\", \"%s\") = %d %s\n", tests[i].pat, tests[i].str,
               result, result == tests[i].expect ? "OK" : "FAIL");
        if (result != tests[i].expect) return 1;
    }
    printf("PASS\n");
    return 0;
}
