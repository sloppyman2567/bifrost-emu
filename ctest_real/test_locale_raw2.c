// test_locale_raw2.c — dump raw locale data using dlsym.
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

int main() {
    // _nl_global_locale is a static symbol, but we can compute it
    // from localeconv() which returns a struct lconv*. The lconv struct
    // is allocated from the locale data, so we can trace back.
    
    write(1, "[1 localeconv] ", 15);
    struct lconv *lc = localeconv();
    printf("lconv @ %p\n", lc);
    if (lc) {
        printf("decimal_point @ %p = '%s'\n", lc->decimal_point, 
               lc->decimal_point ? lc->decimal_point : "(null)");
        printf("thousands_sep @ %p = '%s'\n", lc->thousands_sep,
               lc->thousands_sep ? lc->thousands_sep : "(null)");
    }

    // The decimal_point pointer should point into _nl_C_LC_NUMERIC.
    // If it's NULL, the locale data isn't initialized.
    
    write(1, "[2 setlocale C] ", 16);
    setlocale(LC_ALL, "C");
    lc = localeconv();
    if (lc && lc->decimal_point) {
        printf("after setlocale: decimal_point = '%s' @ %p\n",
               lc->decimal_point, lc->decimal_point);
    } else {
        write(1, "still NULL\n", 11);
    }

    // Try printing a float after setlocale
    write(1, "[3 %.2f after setlocale] ", 25);
    printf("%.2f\n", 3.14);

    write(1, "[done]\n", 7);
    return 0;
}
