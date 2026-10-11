// test_locale_ptr.c — check if locale pointers are properly relocated.
#include <stdio.h>
#include <locale.h>
#include <unistd.h>
#include <stdint.h>

int main() {
    // After setlocale, the _nl_global_locale should have valid pointers.
    setlocale(LC_ALL, "C");
    
    struct lconv *lc = localeconv();
    printf("lconv=%p\n", lc);
    if (lc) {
        // decimal_point is the first field
        printf("decimal_point_ptr=%p\n", lc->decimal_point);
        if (lc->decimal_point) {
            printf("decimal_point='%c' (0x%02x)\n", 
                   lc->decimal_point[0], (unsigned)lc->decimal_point[0]);
        }
    }

    // Direct float print
    printf("float: %.2f\n", 3.14);
    printf("done\n");
    return 0;
}
