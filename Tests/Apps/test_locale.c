// test_locale.c — test glibc locale data access under dynamic linking.
#include <stdio.h>
#include <locale.h>
#include <unistd.h>
#include <string.h>
#include <langinfo.h>

int main() {
    write(1, "[1 localeconv] ", 15);
    struct lconv *lc = localeconv();
    if (lc) {
        printf("decimal_point=%s\n", lc->decimal_point ? lc->decimal_point : "(null)");
        printf("thousands_sep=%s\n", lc->thousands_sep ? lc->thousands_sep : "(null)");
    } else {
        write(1, "localeconv returned NULL\n", 25);
    }

    write(1, "[2 setlocale] ", 14);
    char *loc = setlocale(LC_ALL, "C");
    printf("setlocale returned: %s\n", loc ? loc : "(null)");

    write(1, "[3 nl_langinfo] ", 16);
    char *dp = nl_langinfo(RADIXCHAR);
    printf("RADIXCHAR (decimal point): %s\n", dp ? dp : "(null)");

    write(1, "[done]\n", 7);
    return 0;
}
