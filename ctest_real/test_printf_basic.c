// test_printf_basic.c — minimal printf test to isolate the interp bug
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("Hello\n");
    printf("World\n");
    printf("Before tzset:\n");
    printf("  tzname[0]=%s tzname[1]=%s\n", "UTC", "UTC");
    printf("After tzset:\n");
    printf("  value=%d\n", 42);
    printf("  hex=%x\n", 0xABCD);
    printf("  float=%.2f\n", 3.14);
    printf("DONE\n");
    return 0;
}
