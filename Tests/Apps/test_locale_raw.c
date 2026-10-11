// test_locale_raw.c — dump raw locale data to find the bug.
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

// These are glibc internals — we access them to debug.
extern char _nl_global_locale[];
extern char _nl_C_LC_NUMERIC[];

int main() {
    write(1, "[1 _nl_global_locale] ", 22);
    printf("_nl_global_locale @ %p\n", _nl_global_locale);

    // Dump first 128 bytes of _nl_global_locale
    write(1, "[2 raw dump] ", 13);
    uint64_t *p = (uint64_t*)_nl_global_locale;
    char buf[32];
    for (int i = 0; i < 16; i++) {
        snprintf(buf, sizeof(buf), "+%d:0x%016lx ", i*8, p[i]);
        write(1, buf, strlen(buf));
    }
    write(1, "\n", 1);

    write(1, "[3 _nl_C_LC_NUMERIC] ", 21);
    printf("_nl_C_LC_NUMERIC @ %p\n", _nl_C_LC_NUMERIC);
    
    // Dump first 64 bytes of _nl_C_LC_NUMERIC
    write(1, "[4 numeric dump] ", 17);
    p = (uint64_t*)_nl_C_LC_NUMERIC;
    for (int i = 0; i < 8; i++) {
        snprintf(buf, sizeof(buf), "+%d:0x%016lx ", i*8, p[i]);
        write(1, buf, strlen(buf));
    }
    write(1, "\n", 1);

    // The decimal point should be at _nl_C_LC_NUMERIC + some offset
    // The locale_data struct has: unsigned int nstrings, unsigned int ncategories, ...
    // followed by a table of file offsets. The actual strings are after that.
    // For LC_NUMERIC, the decimal_point is string index 0.
    write(1, "[5 search for '.'] ", 19);
    char *s = (char*)_nl_C_LC_NUMERIC;
    for (int i = 0; i < 256; i++) {
        if (s[i] == '.') {
            snprintf(buf, sizeof(buf), "found '.' at offset %d\n", i);
            write(1, buf, strlen(buf));
            break;
        }
    }

    write(1, "[done]\n", 7);
    return 0;
}
